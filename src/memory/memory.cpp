#include "ok/memory/memory.hpp"
#include "ok/memory/physical_map.hpp"

namespace ok::memory
{
namespace
{

// Sweep byte boundaries before page alignment: reservations always win and
// adjacent usable fragments may together cover a complete physical page.
// `emit(base, count)` receives each eligible page run in ascending order.
template <typename Emit>
Status sweep_usable(std::span<const MemoryRegion> regions, usize page_size, Emit &&emit)
{
    if (page_size == 0 || (page_size & (page_size - 1)) != 0)
        return Status::invalid_argument("page size must be a power of two");
    constexpr uptr address_limit = ~uptr{0};
    uptr cursor = address_limit;
    for (const auto &region : regions)
    {
        if (region.size > address_limit - region.base)
            return Status::overflow("physical memory region wraps the address space");
        if (region.size != 0 && region.base < cursor)
            cursor = region.base;
    }
    auto append = [&](uptr start, uptr end) -> Status {
        const usize adjustment = (page_size - (start % page_size)) % page_size;
        if (adjustment >= end - start)
            return Status::success();
        start += adjustment;
        end -= end % page_size;
        if (end <= start)
            return Status::success();
        return emit(start, (end - start) / page_size);
    };
    bool in_usable_run = false;
    uptr run_start = 0;
    while (true)
    {
        bool usable = false;
        bool reserved = false;
        bool next_found = false;
        uptr next = address_limit;
        for (const auto &region : regions)
        {
            if (region.size == 0)
                continue;
            const uptr end = region.base + region.size;
            if (region.base <= cursor && cursor < end)
            {
                usable |= region.type == RegionType::usable;
                reserved |= region.type != RegionType::usable;
            }
            for (const uptr boundary : {region.base, end})
            {
                if (boundary > cursor && (!next_found || boundary < next))
                {
                    next = boundary;
                    next_found = true;
                }
            }
        }
        if (!next_found)
            break;
        if (usable && !reserved)
        {
            if (!in_usable_run)
            {
                run_start = cursor;
                in_usable_run = true;
            }
        }
        else if (in_usable_run)
        {
            if (auto status = append(run_start, cursor); !status.ok())
                return status;
            in_usable_run = false;
        }
        cursor = next;
    }
    if (in_usable_run)
        return append(run_start, cursor);
    return Status::success();
}

// Branch-based search: several profiles have no count-trailing-zeros
// instruction and the libgcc fallback does not link into every kernel model.
[[nodiscard]] usize lowest_clear_bit(u64 bits)
{
    u64 clear = ~bits;
    usize index = 0;
    for (usize width = 32; width != 0; width /= 2)
    {
        const u64 mask = (u64{1} << width) - 1;
        if ((clear & mask) == 0)
        {
            clear >>= width;
            index += width;
        }
    }
    return index;
}

struct SweepTotals
{
    usize frames{0};
    usize ranges{0};
};

Result<SweepTotals> count_usable(std::span<const MemoryRegion> regions, usize page_size)
{
    SweepTotals totals;
    auto status = sweep_usable(regions, page_size, [&](uptr, usize count) -> Status {
        if (count > ~usize{0} - totals.frames)
            return Status::overflow("physical frame count overflows");
        totals.frames += count;
        ++totals.ranges;
        return Status::success();
    });
    if (!status.ok())
        return status;
    if (totals.frames == 0)
        return Status::no_memory("no complete usable physical frames");
    // The descriptor array must itself be addressable.
    if (totals.frames > (~usize{0} - FrameAllocator::metadata_alignment) / (sizeof(PageDescriptor) + 1) ||
        totals.ranges > ~usize{0} / sizeof(FrameAllocator::FrameRange) / 2)
        return Status::overflow("frame metadata size overflows");
    return totals;
}

} // namespace

Result<usize> FrameAllocator::metadata_bytes(std::span<const MemoryRegion> regions, usize page_size)
{
    auto totals = count_usable(regions, page_size);
    if (!totals)
        return totals.status();
    return metadata_bytes_for(totals.value().frames, totals.value().ranges);
}

Status FrameAllocator::initialize(std::span<const MemoryRegion> regions, usize page_size, std::span<std::byte> metadata)
{
    if (used_count_ != 0)
        return Status::busy("physical frames are still owned");
    auto totals = count_usable(regions, page_size);
    if (!totals)
        return totals.status();
    const auto [frames, range_count] = totals.value();
    if (metadata.data() == nullptr || metadata.size() < metadata_bytes_for(frames, range_count))
        return Status::no_memory("frame allocator metadata storage is too small");

    const uptr start = reinterpret_cast<uptr>(metadata.data());
    const uptr aligned = (start + metadata_alignment - 1) & ~uptr{metadata_alignment - 1};
    std::byte *cursor = metadata.data() + (aligned - start);
    const usize words = (frames + 63) / 64;
    bitmap_ = {reinterpret_cast<u64 *>(cursor), words};
    cursor += words * sizeof(u64);
    descriptors_ = {reinterpret_cast<PageDescriptor *>(cursor), frames};
    cursor += frames * sizeof(PageDescriptor);
    ranges_ = {reinterpret_cast<FrameRange *>(cursor), range_count};
    for (auto &word : bitmap_)
        word = 0;
    // Bits past the last frame stay set so the word scan never yields them.
    if (frames % 64 != 0)
        bitmap_[words - 1] = ~u64{0} << (frames % 64);
    for (auto &descriptor : descriptors_)
        descriptor = {};
    usize index = 0;
    usize range = 0;
    static_cast<void>(sweep_usable(regions, page_size, [&](uptr base, usize count) -> Status {
        ranges_[range++] = FrameRange{.base = base, .first_index = index, .count = count};
        index += count;
        return Status::success();
    }));
    page_size_ = page_size;
    frame_count_ = frames;
    next_free_hint_ = 0;
    // Preserve the generation sequence across reinitialization so old handles stay stale.
    return Status::success();
}

bool FrameAllocator::used(usize index) const
{
    return ((bitmap_[index / 64] >> (index % 64)) & 1u) != 0;
}

void FrameAllocator::mark(usize index, bool value)
{
    const u64 mask = u64{1} << (index % 64);
    if (value)
        bitmap_[index / 64] |= mask;
    else
        bitmap_[index / 64] &= ~mask;
}

uptr FrameAllocator::address_for(usize index) const
{
    usize low = 0;
    usize high = ranges_.size();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        const auto &range = ranges_[middle];
        if (index < range.first_index)
            high = middle;
        else if (index - range.first_index >= range.count)
            low = middle + 1;
        else
            return range.base + (index - range.first_index) * page_size_;
    }
    return 0;
}

Result<usize> FrameAllocator::index_of(uptr address) const
{
    if (frame_count_ == 0)
        return Status::not_initialized("physical frame allocator is not initialized");
    usize low = 0;
    usize high = ranges_.size();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        const auto &range = ranges_[middle];
        if (address < range.base)
            high = middle;
        else if ((address - range.base) / page_size_ >= range.count)
            low = middle + 1;
        else if ((address - range.base) % page_size_ != 0)
            return Status::invalid_argument("frame address is not page aligned");
        else
            return range.first_index + (address - range.base) / page_size_;
    }
    return Status::not_found("address is not a managed physical frame");
}

bool FrameAllocator::exact(const PhysicalFrame &frame) const
{
    return frame.owner == this && frame.index < frame_count_ && frame.generation != 0 &&
           descriptors_[frame.index].generation == frame.generation && address_for(frame.index) == frame.address;
}

PageDescriptor *FrameAllocator::descriptor(const PhysicalFrame &frame)
{
    return exact(frame) ? &descriptors_[frame.index] : nullptr;
}

const PageDescriptor *FrameAllocator::descriptor(const PhysicalFrame &frame) const
{
    return exact(frame) ? &descriptors_[frame.index] : nullptr;
}

Result<PhysicalFrame> FrameAllocator::handle_at(usize index) const
{
    if (index >= frame_count_ || descriptors_[index].generation == 0)
        return Status::not_found("physical frame is not allocated");
    return PhysicalFrame{
        .address = address_for(index), .index = index, .owner = this, .generation = descriptors_[index].generation};
}

PhysicalFrame FrameAllocator::take(usize index)
{
    mark(index, true);
    ++used_count_;
    descriptors_[index] = PageDescriptor{.generation = next_generation_++};
    return PhysicalFrame{
        .address = address_for(index), .index = index, .owner = this, .generation = descriptors_[index].generation};
}

Result<PhysicalFrame> FrameAllocator::allocate()
{
    if (frame_count_ == 0)
        return Status::not_initialized("physical frame allocator is not initialized");
    if (used_count_ >= frame_count_)
        return Status::no_memory("physical frame allocator exhausted");
    if (next_generation_ == 0)
        return Status::overflow("physical frame ownership sequence exhausted");
    const usize words = bitmap_.size();
    for (usize scanned = 0; scanned <= words; ++scanned)
    {
        const usize word = (next_free_hint_ / 64 + scanned) % words;
        const u64 bits = bitmap_[word];
        if (bits == ~u64{0})
            continue;
        const usize index = word * 64 + lowest_clear_bit(bits);
        auto frame = take(index);
        next_free_hint_ = index + 1 < frame_count_ ? index + 1 : 0;
        return frame;
    }
    return Status::no_memory("physical frame allocator exhausted");
}

Result<PhysicalFrame> FrameAllocator::allocate(ChargeAccount &account)
{
    if (frame_count_ == 0)
        return Status::not_initialized("physical frame allocator is not initialized");
    if (auto status = account.charge(1); !status.ok())
        return status;
    auto frame = allocate();
    if (!frame)
        static_cast<void>(account.uncharge(1));
    return frame;
}

Result<PhysicalFrame> FrameAllocator::allocate_contiguous(usize count, ChargeAccount &account)
{
    if (frame_count_ == 0)
        return Status::not_initialized("physical frame allocator is not initialized");
    if (count == 0)
        return Status::invalid_argument("contiguous frame request is empty");
    if (count > frame_count_ - used_count_)
        return Status::no_memory("physical frame allocator exhausted");
    if (count > ~u64{0} - next_generation_)
        return Status::overflow("physical frame ownership sequence exhausted");
    for (const auto &range : ranges_)
    {
        usize run = 0;
        for (usize offset = 0; offset < range.count; ++offset)
        {
            run = used(range.first_index + offset) ? 0 : run + 1;
            if (run != count)
                continue;
            if (auto status = account.charge(count); !status.ok())
                return status;
            const usize first = range.first_index + offset + 1 - count;
            for (usize i = 0; i < count; ++i)
                static_cast<void>(take(first + i));
            return handle_at(first);
        }
    }
    return Status::no_memory("no physically contiguous frame run");
}

Status FrameAllocator::release_contiguous(PhysicalFrame first, usize count, ChargeAccount &account)
{
    if (count == 0 || !exact(first) || count > frame_count_ - first.index)
        return Status::invalid_argument("contiguous frame run handle is invalid or stale");
    for (usize i = 1; i < count; ++i)
        if (!used(first.index + i) || address_for(first.index + i) != first.address + i * page_size_)
            return Status::invalid_argument("contiguous frame run is not wholly owned");
    if (account.used() < count)
        return Status::invalid_argument("contiguous frame release has no matching charge");
    for (usize i = 0; i < count; ++i)
    {
        mark(first.index + i, false);
        descriptors_[first.index + i] = {};
    }
    used_count_ -= count;
    if (first.index < next_free_hint_)
        next_free_hint_ = first.index;
    static_cast<void>(account.uncharge(count));
    return Status::success();
}

Status FrameAllocator::release(PhysicalFrame frame)
{
    if (!exact(frame) || !used(frame.index))
        return Status::invalid_argument("physical frame ownership handle is invalid or stale");
    mark(frame.index, false);
    descriptors_[frame.index] = {};
    --used_count_;
    if (frame.index < next_free_hint_)
        next_free_hint_ = frame.index;
    return Status::success();
}

Status FrameAllocator::release(PhysicalFrame frame, ChargeAccount &account)
{
    if (!exact(frame) || !used(frame.index))
        return Status::invalid_argument("physical frame ownership handle is invalid or stale");
    if (account.used() == 0)
        return Status::invalid_argument("physical frame release has no matching charge");
    if (auto status = release(frame); !status.ok())
        return status;
    static_cast<void>(account.uncharge(1));
    return Status::success();
}

Status MemoryManager::initialize(std::span<const MemoryRegion> regions, usize page_size, std::span<std::byte> metadata)
{
    if (auto status = frames_.initialize(regions, page_size, metadata); !status.ok())
        return status;
    metadata_region_ = {};
    return Status::success();
}

Status MemoryManager::initialize(const PhysicalMemoryMap &map, usize page_size, uptr direct_offset)
{
    if (!map.normalized())
        return Status::not_initialized("physical memory map is not normalized");
    if (page_size == 0 || (page_size & (page_size - 1)) != 0)
        return Status::invalid_argument("page size must be a power of two");
    // One extra slot holds the carved metadata reservation.
    std::array<MemoryRegion, max_discovered_regions + 1> regions{};
    const auto usable = map.usable();
    for (usize i = 0; i < usable.size(); ++i)
    {
        if (usable[i].base > ~uptr{0} - direct_offset || usable[i].size > ~uptr{0} - (usable[i].base + direct_offset))
            return Status::overflow("direct-map address of usable RAM wraps");
        regions[i] = {.base = usable[i].base + direct_offset, .size = usable[i].size, .type = RegionType::usable};
    }
    auto required = FrameAllocator::metadata_bytes({regions.data(), usable.size()}, page_size);
    if (!required)
        return required.status();
    const usize carve = (required.value() + page_size - 1) & ~(page_size - 1);
    // Top-down keeps low RAM for consumers with address limits. The carve must
    // leave at least one managed page behind.
    for (usize i = usable.size(); i-- != 0;)
    {
        if (regions[i].size <= carve)
            continue;
        const MemoryRegion metadata{
            .base = regions[i].base + regions[i].size - carve, .size = carve, .type = RegionType::reserved};
        regions[usable.size()] = metadata;
        if (auto status = frames_.initialize({regions.data(), usable.size() + 1}, page_size,
                                             {reinterpret_cast<std::byte *>(metadata.base), metadata.size});
            !status.ok())
            return status;
        metadata_region_ = metadata;
        return Status::success();
    }
    return Status::no_memory("no usable range can hold the frame allocator metadata");
}

} // namespace ok::memory
