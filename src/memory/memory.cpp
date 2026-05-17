#include "ok/memory/memory.hpp"

#include <algorithm>

namespace ok::memory {

Status FrameAllocator::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    if (page_size == 0) {
        return Status::invalid_argument("page size must be non-zero");
    }

    page_size_ = page_size;
    uptr lowest = 0;
    uptr highest = 0;
    bool found = false;

    for (const auto& region : regions) {
        if (region.type != RegionType::usable || region.size < page_size_) {
            continue;
        }
        const uptr start = (region.base + page_size_ - 1) / page_size_ * page_size_;
        const uptr end = (region.base + region.size) / page_size_ * page_size_;
        if (start >= end) {
            continue;
        }
        if (!found || start < lowest) {
            lowest = start;
        }
        highest = std::max(highest, end);
        found = true;
    }

    if (!found) {
        return Status::no_memory("no usable physical memory region");
    }

    base_ = lowest;
    const usize frame_count = (highest - lowest) / page_size_;
    frame_used_.assign(frame_count, true);

    for (const auto& region : regions) {
        if (region.type != RegionType::usable) {
            continue;
        }
        const uptr start = (region.base + page_size_ - 1) / page_size_ * page_size_;
        const uptr end = (region.base + region.size) / page_size_ * page_size_;
        for (uptr address = start; address < end; address += page_size_) {
            const usize index = (address - base_) / page_size_;
            if (index < frame_used_.size()) {
                frame_used_[index] = false;
            }
        }
    }

    return Status::success();
}

Result<PhysicalFrame> FrameAllocator::allocate()
{
    for (usize index = 0; index < frame_used_.size(); ++index) {
        if (!frame_used_[index]) {
            frame_used_[index] = true;
            return PhysicalFrame {.address = base_ + index * page_size_, .index = index};
        }
    }
    return Status::no_memory("physical frame allocator exhausted");
}

Status FrameAllocator::release(PhysicalFrame frame)
{
    if (frame.index >= frame_used_.size()) {
        return Status::invalid_argument("physical frame index out of range");
    }
    if (!frame_used_[frame.index]) {
        return Status::invalid_argument("physical frame already free");
    }
    frame_used_[frame.index] = false;
    return Status::success();
}

usize FrameAllocator::free_frames() const
{
    return static_cast<usize>(std::count(frame_used_.begin(), frame_used_.end(), false));
}

Status LinearAddressSpace::map(uptr virtual_address, PhysicalFrame frame, usize flags)
{
    const auto exists = std::any_of(mappings_.begin(), mappings_.end(), [virtual_address](const Mapping& mapping) {
        return mapping.virtual_address == virtual_address;
    });
    if (exists) {
        return Status::already_exists("virtual address already mapped");
    }
    mappings_.push_back(Mapping {.virtual_address = virtual_address, .frame = frame, .flags = flags});
    return Status::success();
}

Status LinearAddressSpace::unmap(uptr virtual_address)
{
    const auto old_size = mappings_.size();
    std::erase_if(mappings_, [virtual_address](const Mapping& mapping) {
        return mapping.virtual_address == virtual_address;
    });
    if (mappings_.size() == old_size) {
        return Status::not_found("virtual address not mapped");
    }
    return Status::success();
}

Status MemoryManager::initialize(std::span<const MemoryRegion> regions, usize page_size)
{
    return frames_.initialize(regions, page_size);
}

} // namespace ok::memory

