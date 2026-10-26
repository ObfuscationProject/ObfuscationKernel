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

[[nodiscard]] usize lowest_clear_bit(u64 bits)
{
    const u64 clear = ~bits;
    const auto low = static_cast<u32>(clear);
    return low != 0 ? static_cast<usize>(__builtin_ctz(low)) : 32 + static_cast<usize>(__builtin_ctz(static_cast<u32>(clear >> 32)));
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

Status FrameAllocator::allocate_contiguous(usize count, std::span<PhysicalFrame> output, ChargeAccount &account)
{
    if (frame_count_ == 0)
        return Status::not_initialized("physical frame allocator is not initialized");
    if (count == 0 || output.size() < count)
        return Status::invalid_argument("contiguous frame request has no complete output");
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
                output[i] = take(first + i);
            return Status::success();
        }
    }
    return Status::no_memory("no physically contiguous frame run");
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

Status LinearAddressSpace::map(uptr virtual_address, PhysicalFrame frame, usize flags)
{
    for (const auto &mapping : mappings_)
    {
        if (mapping.virtual_address == virtual_address)
        {
            return Status::already_exists("virtual address already mapped");
        }
    }
    return mappings_.push_back(Mapping{.virtual_address = virtual_address, .frame = frame, .flags = flags});
}

Status LinearAddressSpace::unmap(uptr virtual_address)
{
    for (usize i = 0; i < mappings_.size(); ++i)
    {
        if (mappings_[i].virtual_address == virtual_address)
        {
            return mappings_.erase_at(i);
        }
    }
    return Status::not_found("virtual address not mapped");
}

namespace
{

uptr page_base(uptr address)
{
    return address / vm_test_page_size * vm_test_page_size;
}

usize page_offset(uptr address)
{
    return static_cast<usize>(address - page_base(address));
}

} // namespace

Status PageTable::map(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    if ((virtual_address % vm_test_page_size) != 0)
    {
        return Status::invalid_argument("virtual address is not page aligned");
    }
    for (const auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == virtual_address)
        {
            return Status::already_exists("page is already mapped");
        }
    }
    return entries_.push_back(PageTableEntry{
        .virtual_address = virtual_address,
        .frame = frame,
        .permissions = permissions,
        .present = true,
    });
}

Status PageTable::unmap(uptr virtual_address)
{
    const auto base = page_base(virtual_address);
    for (usize i = 0; i < entries_.size(); ++i)
    {
        if (entries_[i].present && entries_[i].virtual_address == base)
        {
            return entries_.erase_at(i);
        }
    }
    return Status::not_found("page is not mapped");
}

PageTableEntry *PageTable::lookup(uptr virtual_address)
{
    const auto base = page_base(virtual_address);
    for (auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == base)
        {
            return &entry;
        }
    }
    return nullptr;
}

const PageTableEntry *PageTable::lookup(uptr virtual_address) const
{
    const auto base = page_base(virtual_address);
    for (const auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == base)
        {
            return &entry;
        }
    }
    return nullptr;
}

Status KernelAddressSpace::map_page(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    return table_.map(virtual_address, frame, permissions | page_global);
}

Status KernelAddressSpace::unmap_page(uptr virtual_address)
{
    return table_.unmap(virtual_address);
}

UserAddressSpace::UserPage *UserAddressSpace::page_for(uptr address)
{
    const auto base = page_base(address);
    for (auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            return &page;
        }
    }
    return nullptr;
}

const UserAddressSpace::UserPage *UserAddressSpace::page_for(uptr address) const
{
    const auto base = page_base(address);
    for (const auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            return &page;
        }
    }
    return nullptr;
}

Status UserAddressSpace::map_page(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    if ((permissions & page_user) == 0)
    {
        return Status::invalid_argument("user page mapping requires user permission");
    }
    if (auto status = table_.map(virtual_address, frame, permissions); !status.ok())
    {
        return status;
    }
    for (auto &page : pages_)
    {
        if (!page.used)
        {
            page = UserPage{.used = true, .virtual_address = virtual_address, .permissions = permissions};
            return Status::success();
        }
    }
    static_cast<void>(table_.unmap(virtual_address));
    return Status::overflow("user page table capacity exceeded");
}

Status UserAddressSpace::unmap_page(uptr virtual_address)
{
    const auto base = page_base(virtual_address);
    for (auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            page = {};
            return table_.unmap(base);
        }
    }
    return Status::not_found("user page is not mapped");
}

Status UserAddressSpace::clone_metadata_from(const UserAddressSpace &source)
{
    table_ = {};
    pages_ = {};
    for (const auto &entry : source.table_.entries())
    {
        if (!entry.present)
        {
            continue;
        }
        if (auto status = map_page(entry.virtual_address, entry.frame, entry.permissions); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status UserAddressSpace::mark_copy_on_write(uptr virtual_address)
{
    auto *entry = table_.lookup(virtual_address);
    auto *page = page_for(virtual_address);
    if (entry == nullptr || page == nullptr)
    {
        return Status::not_found("copy-on-write page is not mapped");
    }
    entry->permissions = (entry->permissions & ~page_write) | page_copy_on_write;
    page->permissions = entry->permissions;
    return Status::success();
}

Status UserAddressSpace::validate_range(uptr address, usize size, usize required_permissions) const
{
    if (size == 0)
    {
        return Status::success();
    }
    if (address == 0)
    {
        return Status::invalid_argument("user pointer is null");
    }
    for (usize cursor = 0; cursor < size;)
    {
        const auto current = address + cursor;
        const auto *page = page_for(current);
        if (page == nullptr)
        {
            return Status::fault("user pointer is unmapped");
        }
        if ((page->permissions & page_user) == 0 || (page->permissions & required_permissions) != required_permissions)
        {
            return Status::denied("user pointer permissions do not allow access");
        }
        const usize available = vm_test_page_size - page_offset(current);
        const usize step = (size - cursor) < available ? (size - cursor) : available;
        cursor += step;
    }
    return Status::success();
}

bool UserAddressSpace::valid(UserSlice<const std::byte> slice, usize required_permissions) const
{
    return validate_range(slice.address, slice.count, required_permissions).ok();
}

CopyResult UserAddressSpace::copy_from_user(UserSlice<const std::byte> source,
                                            std::span<std::byte> destination) const
{
    const usize count = source.count < destination.size() ? source.count : destination.size();
    if (auto status = validate_range(source.address, count, page_read); !status.ok())
    {
        return CopyResult{.bytes = 0, .status = status};
    }
    for (usize copied = 0; copied < count;)
    {
        const auto current = source.address + copied;
        const auto *page = page_for(current);
        const usize offset = page_offset(current);
        const usize available = vm_test_page_size - offset;
        const usize step = (count - copied) < available ? (count - copied) : available;
        for (usize i = 0; i < step; ++i)
        {
            destination[copied + i] = page->data[offset + i];
        }
        copied += step;
    }
    return CopyResult{.bytes = count, .status = Status::success()};
}

CopyResult UserAddressSpace::copy_to_user(UserSlice<std::byte> destination, std::span<const std::byte> source)
{
    const usize count = destination.count < source.size() ? destination.count : source.size();
    if (auto status = validate_range(destination.address, count, page_write); !status.ok())
    {
        return CopyResult{.bytes = 0, .status = status};
    }
    for (usize copied = 0; copied < count;)
    {
        const auto current = destination.address + copied;
        auto *page = page_for(current);
        const usize offset = page_offset(current);
        const usize available = vm_test_page_size - offset;
        const usize step = (count - copied) < available ? (count - copied) : available;
        for (usize i = 0; i < step; ++i)
        {
            page->data[offset + i] = source[copied + i];
        }
        copied += step;
    }
    return CopyResult{.bytes = count, .status = Status::success()};
}

Result<FixedString<256>> UserAddressSpace::copy_c_string_from_user(UserPtr<const char> source, usize max_length) const
{
    if (source.null())
    {
        return Status::invalid_argument("user string pointer is null");
    }
    FixedString<256> out;
    const usize limit = max_length < 255 ? max_length : 255;
    for (usize i = 0; i < limit; ++i)
    {
        const auto current = source.address + i;
        if (auto status = validate_range(current, 1, page_read); !status.ok())
        {
            return status;
        }
        const auto *page = page_for(current);
        const char value = static_cast<char>(page->data[page_offset(current)]);
        if (value == '\0')
        {
            return out;
        }
        if (auto status = out.append(value); !status.ok())
        {
            return status;
        }
    }
    return Status::overflow("user string is not nul-terminated within bound");
}

Status VirtualMemoryManager::initialize(FrameAllocator &frames)
{
    frames_ = &frames;
    return Status::success();
}

Result<PhysicalFrame> VirtualMemoryManager::allocate_user_frame()
{
    if (frames_ == nullptr)
    {
        return Status::not_initialized("virtual memory manager has no frame allocator");
    }
    return frames_->allocate();
}

PageFaultKind classify_page_fault(bool present, bool write, bool user, bool execute, usize permissions)
{
    if (!present)
    {
        return PageFaultKind::not_present;
    }
    if (user && (permissions & page_user) == 0)
    {
        return PageFaultKind::user_access;
    }
    if (write && (permissions & page_write) == 0)
    {
        return PageFaultKind::write_to_read_only;
    }
    if (execute && (permissions & page_execute) == 0)
    {
        return PageFaultKind::execute_disabled;
    }
    return PageFaultKind::protection;
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
