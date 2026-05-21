#include "ok/memory/memory.hpp"

namespace ok::memory
{

Status FrameAllocator::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    if (page_size == 0)
    {
        return Status::invalid_argument("page size must be non-zero");
    }

    page_size_ = page_size;
    uptr lowest = 0;
    uptr highest = 0;
    bool found = false;

    for (const auto &region : regions)
    {
        if (region.type != RegionType::usable || region.size < page_size_)
        {
            continue;
        }
        const uptr start = (region.base + page_size_ - 1) / page_size_ * page_size_;
        const uptr end = (region.base + region.size) / page_size_ * page_size_;
        if (start >= end)
        {
            continue;
        }
        if (!found || start < lowest)
        {
            lowest = start;
        }
        if (end > highest)
        {
            highest = end;
        }
        found = true;
    }

    if (!found)
    {
        return Status::no_memory("no usable physical memory region");
    }

    base_ = lowest;
    const usize frame_count = (highest - lowest) / page_size_;
    if (frame_count > max_physical_frames)
    {
        return Status::overflow("physical frame table capacity exceeded");
    }
    frame_count_ = frame_count;
    used_count_ = frame_count_;
    next_free_hint_ = 0;
    for (usize i = 0; i < frame_count_; ++i)
    {
        frame_used_[i] = true;
    }

    for (const auto &region : regions)
    {
        if (region.type != RegionType::usable)
        {
            continue;
        }
        const uptr start = (region.base + page_size_ - 1) / page_size_ * page_size_;
        const uptr end = (region.base + region.size) / page_size_ * page_size_;
        for (uptr address = start; address < end; address += page_size_)
        {
            const usize index = (address - base_) / page_size_;
            if (index < frame_count_)
            {
                if (frame_used_[index])
                {
                    frame_used_[index] = false;
                    --used_count_;
                    if (index < next_free_hint_)
                    {
                        next_free_hint_ = index;
                    }
                }
            }
        }
    }

    return Status::success();
}

Result<PhysicalFrame> FrameAllocator::allocate()
{
    if (used_count_ >= frame_count_)
    {
        return Status::no_memory("physical frame allocator exhausted");
    }

    for (usize offset = 0; offset < frame_count_; ++offset)
    {
        const usize index = (next_free_hint_ + offset) % frame_count_;
        if (!frame_used_[index])
        {
            frame_used_[index] = true;
            ++used_count_;
            next_free_hint_ = (index + 1) % frame_count_;
            return PhysicalFrame{.address = base_ + index * page_size_, .index = index};
        }
    }
    return Status::no_memory("physical frame allocator exhausted");
}

Status FrameAllocator::release(PhysicalFrame frame)
{
    if (frame.index >= frame_count_)
    {
        return Status::invalid_argument("physical frame index out of range");
    }
    if (!frame_used_[frame.index])
    {
        return Status::invalid_argument("physical frame already free");
    }
    frame_used_[frame.index] = false;
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
