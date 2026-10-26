#include "ok/memory/slab.hpp"
#include "ok/memory/memory.hpp"

#include <new>

namespace ok::memory
{
namespace
{

constexpr usize slab_page_size = 4096;
constexpr usize header_bytes = 64;
constexpr usize bitmap_words = 4;

// In-page header of a slab page. Set bitmap bits are allocated slots.
struct SlabHeader
{
    uptr next{};
    uptr previous{};
    u32 free{};
    u16 slots{};
    u16 class_index{};
    std::array<u64, bitmap_words> bitmap{};
};
static_assert(sizeof(SlabHeader) <= header_bytes);

constexpr usize first_offset(usize index)
{
    const usize size = SlabAllocator::size_classes[index];
    return size > header_bytes ? size : header_bytes;
}

constexpr usize slots_for(usize index)
{
    return (slab_page_size - first_offset(index)) / SlabAllocator::size_classes[index];
}
static_assert(slots_for(0) <= bitmap_words * 64 && slots_for(SlabAllocator::size_classes.size() - 1) >= 2);

SlabHeader &header(uptr page)
{
    return *std::launder(reinterpret_cast<SlabHeader *>(page));
}

void zero(uptr address, usize size)
{
    auto *bytes = reinterpret_cast<std::byte *>(address);
    for (usize i = 0; i < size; ++i)
        bytes[i] = std::byte{};
}

usize pages_for(usize size)
{
    return size / slab_page_size + (size % slab_page_size != 0);
}

} // namespace

Status SlabAllocator::initialize(FrameAllocator &frames, usize page_limit, usize reserve_frames)
{
    if (frames_ != nullptr)
        return Status::already_exists("slab allocator is already initialized");
    if (!frames.initialized() || frames.page_size() != slab_page_size)
        return Status::invalid_argument("slab allocator requires an initialized 4 KiB frame allocator");
    if (reserve_frames >= frames.total_frames())
        return Status::invalid_argument("slab frame reserve covers all frames");
    if (auto status = pages_.set_limit(page_limit); !status.ok())
        return status;
    frames_ = &frames;
    reserve_ = reserve_frames;
    return Status::success();
}

usize SlabAllocator::class_index(usize size)
{
    for (usize index = 0; index < size_classes.size(); ++index)
        if (size <= size_classes[index])
            return index;
    return size_classes.size();
}

usize SlabAllocator::charge_for(usize size) const
{
    if (size == 0)
        return 0;
    const usize index = class_index(size);
    if (index < size_classes.size())
        return size_classes[index];
    const usize pages = pages_for(size);
    return pages > ~usize{0} / slab_page_size ? 0 : pages * slab_page_size;
}

Status SlabAllocator::reserve_frames_for(usize count, AllocationContext context) const
{
    const usize free = frames_->free_frames();
    if (count > free)
        return Status::no_memory("physical frames exhausted");
    if (context != AllocationContext::atomic && free - count < reserve_)
        return Status::no_memory("frame reserve is held for atomic allocations");
    return Status::success();
}

void SlabAllocator::push(usize index, uptr page)
{
    auto &slab = header(page);
    slab.previous = 0;
    slab.next = classes_[index].partial;
    if (slab.next)
        header(slab.next).previous = page;
    classes_[index].partial = page;
}

void SlabAllocator::unlink(usize index, uptr page)
{
    auto &slab = header(page);
    if (slab.previous)
        header(slab.previous).next = slab.next;
    else
        classes_[index].partial = slab.next;
    if (slab.next)
        header(slab.next).previous = slab.previous;
    slab.next = slab.previous = 0;
}

Result<uptr> SlabAllocator::new_page(usize index, AllocationContext context)
{
    if (auto status = reserve_frames_for(1, context); !status.ok())
        return status;
    auto frame = frames_->allocate(pages_);
    if (!frame)
        return frame.status();
    auto *descriptor = frames_->descriptor(frame.value());
    descriptor->kind = static_cast<u8>(PageKind::kernel_slab);
    descriptor->flags = static_cast<u8>(index);
    const uptr page = frame.value().address;
    zero(page, slab_page_size);
    auto *slab = ::new (reinterpret_cast<void *>(page)) SlabHeader{};
    slab->slots = static_cast<u16>(slots_for(index));
    slab->free = slab->slots;
    slab->class_index = static_cast<u16>(index);
    push(index, page);
    ++classes_[index].pages;
    return page;
}

Result<PhysicalFrame> SlabAllocator::owned_page(uptr address, PageKind kind) const
{
    auto index = frames_->index_of(address);
    if (!index)
        return Status::invalid_argument("object is not in a managed frame");
    auto frame = frames_->handle_at(index.value());
    if (!frame)
        return Status::invalid_argument("object frame is not allocated");
    const auto *descriptor = frames_->descriptor(frame.value());
    if (!descriptor || descriptor->kind != static_cast<u8>(kind))
        return Status::invalid_argument("object frame belongs to another owner");
    return frame;
}

Result<void *> SlabAllocator::allocate(usize size, ChargeAccount &owner, AllocationContext context)
{
    auto result = [&]() -> Result<void *> {
        if (frames_ == nullptr)
            return Status::not_initialized("slab allocator is not initialized");
        if (size == 0)
            return Status::invalid_argument("zero-sized kernel object");
        if (size > large_threshold)
            return allocate_large(size, owner, context);
        const usize index = class_index(size);
        const usize object_size = size_classes[index];
        if (auto status = owner.charge(object_size); !status.ok())
            return status;
        uptr page = classes_[index].partial;
        if (page == 0)
        {
            auto created = new_page(index, context);
            if (!created)
            {
                static_cast<void>(owner.uncharge(object_size));
                return created.status();
            }
            page = created.value();
        }
        auto &slab = header(page);
        for (usize slot = 0; slot < slab.slots; ++slot)
        {
            const u64 bit = u64{1} << (slot % 64);
            if (slab.bitmap[slot / 64] & bit)
                continue;
            slab.bitmap[slot / 64] |= bit;
            if (--slab.free == 0)
                unlink(index, page);
            ++frames_->descriptor(frames_->handle_at(frames_->index_of(page).value()).value())->references;
            ++classes_[index].live;
            const uptr object = page + first_offset(index) + slot * object_size;
            zero(object, object_size);
            return reinterpret_cast<void *>(object);
        }
        // Free counts and slot bits change together; a mismatch is corruption.
        static_cast<void>(owner.uncharge(object_size));
        return Status::fault("slab free count disagrees with its slot bitmap");
    }();
    if (result)
        ++allocations_[static_cast<usize>(context)];
    else
        ++failures_;
    return result;
}

Status SlabAllocator::release(void *object, usize size, ChargeAccount &owner)
{
    if (frames_ == nullptr)
        return Status::not_initialized("slab allocator is not initialized");
    if (object == nullptr || size == 0)
        return Status::invalid_argument("null or zero-sized kernel object release");
    if (size > large_threshold)
        return release_large(object, size, owner);
    const usize index = class_index(size);
    const usize object_size = size_classes[index];
    const auto address = reinterpret_cast<uptr>(object);
    const uptr page = address & ~uptr{slab_page_size - 1};
    auto frame = owned_page(page, PageKind::kernel_slab);
    if (!frame)
        return frame.status();
    auto *descriptor = frames_->descriptor(frame.value());
    if (descriptor->flags != index)
        return Status::invalid_argument("kernel object size does not match its slab class");
    auto &slab = header(page);
    const usize offset = address - page;
    if (offset < first_offset(index) || (offset - first_offset(index)) % object_size != 0)
        return Status::invalid_argument("kernel object pointer is not a slot start");
    const usize slot = (offset - first_offset(index)) / object_size;
    const u64 bit = u64{1} << (slot % 64);
    if (slot >= slab.slots || !(slab.bitmap[slot / 64] & bit))
        return Status::invalid_argument("kernel object slot is not allocated");
    if (owner.used() < object_size)
        return Status::invalid_argument("kernel object owner holds no matching charge");
    slab.bitmap[slot / 64] &= ~bit;
    if (slab.free++ == 0)
        push(index, page);
    --descriptor->references;
    --classes_[index].live;
    static_cast<void>(owner.uncharge(object_size));
    ++releases_;
    // Keep one partially used page per class; return the rest when empty.
    if (slab.free == slab.slots && (classes_[index].partial != page || slab.next != 0))
    {
        unlink(index, page);
        if (auto status = frames_->release(frame.value(), pages_); !status.ok())
        {
            push(index, page);
            return status;
        }
        --classes_[index].pages;
    }
    return Status::success();
}

Result<void *> SlabAllocator::allocate_large(usize size, ChargeAccount &owner, AllocationContext context)
{
    const usize bytes = charge_for(size);
    if (bytes == 0)
        return Status::overflow("kernel object size overflows");
    const usize count = bytes / slab_page_size;
    if (count > ~u32{0})
        return Status::overflow("kernel object spans too many frames");
    if (auto status = reserve_frames_for(count, context); !status.ok())
        return status;
    if (auto status = owner.charge(bytes); !status.ok())
        return status;
    auto first = frames_->allocate_contiguous(count, pages_);
    if (!first)
    {
        static_cast<void>(owner.uncharge(bytes));
        return first.status();
    }
    for (usize i = 0; i < count; ++i)
    {
        auto *descriptor = frames_->descriptor(frames_->handle_at(first.value().index + i).value());
        descriptor->kind = static_cast<u8>(PageKind::kernel_large);
        descriptor->flags = i == 0;
    }
    frames_->descriptor(first.value())->references = static_cast<u32>(count);
    zero(first.value().address, bytes);
    ++large_allocations_;
    large_pages_ += count;
    return reinterpret_cast<void *>(first.value().address);
}

Status SlabAllocator::release_large(void *object, usize size, ChargeAccount &owner)
{
    const usize bytes = charge_for(size);
    auto first = owned_page(reinterpret_cast<uptr>(object), PageKind::kernel_large);
    if (!first)
        return first.status();
    const auto *descriptor = frames_->descriptor(first.value());
    if (bytes == 0 || descriptor->flags != 1 || descriptor->references != bytes / slab_page_size)
        return Status::invalid_argument("kernel object size does not match its frame run");
    if (owner.used() < bytes)
        return Status::invalid_argument("kernel object owner holds no matching charge");
    const usize count = descriptor->references;
    if (auto status = frames_->release_contiguous(first.value(), count, pages_); !status.ok())
        return status;
    static_cast<void>(owner.uncharge(bytes));
    --large_allocations_;
    large_pages_ -= count;
    ++releases_;
    return Status::success();
}

SlabAccounting SlabAllocator::accounting() const
{
    SlabAccounting result{.large_allocations = large_allocations_,
                          .large_pages = large_pages_,
                          .pages = pages_.used(),
                          .peak_pages = pages_.peak(),
                          .allocations = allocations_,
                          .releases = releases_,
                          .failures = failures_};
    for (usize index = 0; index < size_classes.size(); ++index)
        result.classes[index] = {.object_size = size_classes[index],
                                 .slots_per_page = slots_for(index),
                                 .pages = classes_[index].pages,
                                 .live_objects = classes_[index].live};
    return result;
}

Status MemoryManager::initialize_objects()
{
    if (metadata_region_.size == 0)
        return Status::not_initialized("kernel objects require boot-discovered frames");
    // Policy from discovered RAM: kernel objects hold at most a quarter of the
    // frames, and 1/128 of them (16..256) stays free for atomic callers.
    const usize total = frames_.total_frames();
    usize reserve = total / 128;
    reserve = reserve < 16 ? 16 : reserve > 256 ? 256 : reserve;
    if (reserve >= total)
        reserve = 0;
    return objects_.initialize(frames_, total / 4 ? total / 4 : 1, reserve);
}

} // namespace ok::memory
