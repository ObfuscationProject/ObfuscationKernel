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

Status MemoryManager::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    return frames_.initialize(regions, page_size);
}

} // namespace ok::memory
