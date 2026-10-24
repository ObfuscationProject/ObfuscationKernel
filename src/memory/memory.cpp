#include "ok/memory/memory.hpp"

namespace ok::memory
{

Status FrameAllocator::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    if (used_count_ != 0)
    {
        return Status::busy("physical frames are still owned");
    }
    if (page_size == 0 || (page_size & (page_size - 1)) != 0)
    {
        return Status::invalid_argument("page size must be a power of two");
    }

    constexpr uptr address_limit = ~uptr{0};
    uptr cursor = address_limit;
    for (const auto &region : regions)
    {
        if (region.size > address_limit - region.base)
        {
            return Status::overflow("physical memory region wraps the address space");
        }
        if (region.size != 0 && region.base < cursor)
        {
            cursor = region.base;
        }
    }

    std::array<FrameRange, max_physical_frame_ranges> candidate_ranges{};
    usize candidate_range_count = 0;
    usize candidate_frame_count = 0;
    auto append_range = [&](uptr start, uptr end) -> Status {
        const usize adjustment = (page_size - (start % page_size)) % page_size;
        if (adjustment >= end - start)
        {
            return Status::success();
        }
        start += adjustment;
        end -= end % page_size;
        if (end <= start)
        {
            return Status::success();
        }
        const usize count = (end - start) / page_size;
        if (count > max_physical_frames - candidate_frame_count)
        {
            return Status::overflow("bootstrap physical frame capacity exceeded");
        }
        if (candidate_range_count == candidate_ranges.size())
        {
            return Status::overflow("bootstrap physical range capacity exceeded");
        }
        candidate_ranges[candidate_range_count++] =
            FrameRange{.base = start, .first_index = candidate_frame_count, .count = count};
        candidate_frame_count += count;
        return Status::success();
    };

    // Sweep byte boundaries before page alignment: reservations always win and
    // adjacent usable fragments may together cover a complete physical page.
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
            {
                continue;
            }
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
        {
            break;
        }
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
            if (auto status = append_range(run_start, cursor); !status.ok())
            {
                return status;
            }
            in_usable_run = false;
        }
        cursor = next;
    }
    if (in_usable_run)
    {
        if (auto status = append_range(run_start, cursor); !status.ok())
        {
            return status;
        }
    }
    if (candidate_frame_count == 0)
    {
        return Status::no_memory("no complete usable physical frames");
    }

    ranges_ = candidate_ranges;
    range_count_ = candidate_range_count;
    frame_count_ = candidate_frame_count;
    page_size_ = page_size;
    next_free_hint_ = 0;
    generations_.fill(0);
    // Preserve the sequence across reinitialization so old handles stay stale.
    return Status::success();
}

uptr FrameAllocator::address_for(usize index) const
{
    for (usize i = 0; i < range_count_; ++i)
    {
        const auto &range = ranges_[i];
        if (index >= range.first_index && index - range.first_index < range.count)
        {
            return range.base + (index - range.first_index) * page_size_;
        }
    }
    return 0;
}

Result<PhysicalFrame> FrameAllocator::allocate()
{
    if (frame_count_ == 0)
    {
        return Status::not_initialized("physical frame allocator is not initialized");
    }
    if (used_count_ >= frame_count_)
    {
        return Status::no_memory("physical frame allocator exhausted");
    }
    if (next_generation_ == 0)
    {
        return Status::overflow("physical frame ownership sequence exhausted");
    }

    for (usize offset = 0; offset < frame_count_; ++offset)
    {
        const usize index = (next_free_hint_ + offset) % frame_count_;
        if (generations_[index] == 0)
        {
            const u64 generation = next_generation_++;
            generations_[index] = generation;
            ++used_count_;
            next_free_hint_ = (index + 1) % frame_count_;
            return PhysicalFrame{
                .address = address_for(index), .index = index, .owner = this, .generation = generation};
        }
    }
    return Status::no_memory("physical frame allocator exhausted");
}

Status FrameAllocator::release(PhysicalFrame frame)
{
    if (frame.owner != this || frame.index >= frame_count_ || frame.generation == 0 ||
        generations_[frame.index] != frame.generation || address_for(frame.index) != frame.address)
    {
        return Status::invalid_argument("physical frame ownership handle is invalid or stale");
    }
    generations_[frame.index] = 0;
    --used_count_;
    if (frame.index < next_free_hint_)
    {
        next_free_hint_ = frame.index;
    }
    return Status::success();
}

usize FrameAllocator::free_frames() const
{
    return frame_count_ - used_count_;
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

Status MemoryManager::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    return frames_.initialize(regions, page_size);
}

} // namespace ok::memory
