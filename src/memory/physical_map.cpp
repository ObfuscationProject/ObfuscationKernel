#include "ok/memory/physical_map.hpp"

namespace ok::memory
{
namespace
{

[[nodiscard]] bool range_end(uptr base, usize size, uptr &end)
{
    if (size == 0)
        return false;
    if (base > ~uptr{0} - (size - 1))
        return false;
    end = base + size; // may equal 0 on wrap of an exact top-of-space range; callers compare carefully
    return base + (size - 1) >= base;
}

[[nodiscard]] uptr align_down(uptr value, usize alignment)
{
    return value & ~(static_cast<uptr>(alignment) - 1);
}

[[nodiscard]] uptr align_up_checked(uptr value, usize alignment, bool &overflow)
{
    const uptr mask = static_cast<uptr>(alignment) - 1;
    if (value > ~uptr{0} - mask)
    {
        overflow = true;
        return value;
    }
    return (value + mask) & ~mask;
}

} // namespace

Status PhysicalMemoryMap::add(MemoryRegion region, MapSource source)
{
    if (region.size == 0)
        return Status::success();
    uptr end{};
    if (!range_end(region.base, region.size, end))
        return Status{StatusCode::invalid_argument, "memory region exceeds the address space"};
    if (input_count_ == inputs_.size())
        return Status{StatusCode::overflow, "physical memory map input capacity exhausted"};
    inputs_[input_count_++] = {region, source};
    normalized_ = false;
    return Status::success();
}

Status PhysicalMemoryMap::reserve(uptr base, usize size, RegionType type)
{
    if (type == RegionType::usable)
        return Status{StatusCode::invalid_argument, "reserve requires a non-usable region type"};
    return add({base, size, type}, MapSource::linker);
}

Status PhysicalMemoryMap::normalize(usize page_size)
{
    if (page_size == 0 || (page_size & (page_size - 1)) != 0)
        return Status{StatusCode::invalid_argument, "page size must be a power of two"};
    // Collect usable candidates, page-aligned inward.
    std::array<MemoryRegion, max_discovered_regions> work{};
    usize count = 0;
    for (usize i = 0; i < input_count_; ++i)
    {
        const auto &in = inputs_[i].region;
        if (in.type != RegionType::usable)
            continue;
        bool overflow = false;
        const uptr start = align_up_checked(in.base, page_size, overflow);
        if (overflow)
            continue;
        const uptr last = in.base + (in.size - 1);
        const uptr end_aligned = align_down(last, page_size); // first byte of the last complete page
        if (end_aligned < start)
            continue;
        const usize size = static_cast<usize>(end_aligned - start) + page_size;
        if (count == work.size())
            return Status{StatusCode::overflow, "too many usable regions"};
        work[count++] = {start, size, RegionType::usable};
    }
    // Subtract every reservation (page-aligned outward).
    for (usize i = 0; i < input_count_; ++i)
    {
        const auto &res = inputs_[i].region;
        if (res.type == RegionType::usable)
            continue;
        const uptr rstart = align_down(res.base, page_size);
        const uptr rlast = res.base + (res.size - 1);
        bool overflow = false;
        uptr rend = align_up_checked(rlast, page_size, overflow); // exclusive, unless overflow
        const bool to_top = overflow || rend == 0;
        for (usize w = 0; w < count;)
        {
            const uptr wstart = work[w].base;
            const uptr wend = work[w].base + work[w].size; // exclusive; 0 means top of space
            const bool w_to_top = wend == 0;
            const bool before = !w_to_top && wend <= rstart;
            const bool after = !to_top && rend <= wstart;
            if (before || after)
            {
                ++w;
                continue;
            }
            // Overlap: split into up to two pieces.
            MemoryRegion left{}, right{};
            bool has_left = rstart > wstart;
            bool has_right = !to_top && (w_to_top || rend < wend);
            if (has_left)
                left = {wstart, static_cast<usize>(rstart - wstart), RegionType::usable};
            if (has_right)
                right = {rend, w_to_top ? static_cast<usize>(0 - rend) : static_cast<usize>(wend - rend), RegionType::usable};
            // Remove w.
            for (usize k = w + 1; k < count; ++k)
                work[k - 1] = work[k];
            --count;
            if (has_left)
            {
                if (count == work.size())
                    return Status{StatusCode::overflow, "too many usable fragments after reservation"};
                work[count++] = left;
            }
            if (has_right)
            {
                if (count == work.size())
                    return Status{StatusCode::overflow, "too many usable fragments after reservation"};
                work[count++] = right;
            }
            // Restart scan for this reservation; fragments were appended.
            w = 0;
        }
    }
    // Sort by base (insertion sort; count <= 64) and merge adjacent/overlapping.
    for (usize i = 1; i < count; ++i)
    {
        MemoryRegion key = work[i];
        usize j = i;
        while (j > 0 && work[j - 1].base > key.base)
        {
            work[j] = work[j - 1];
            --j;
        }
        work[j] = key;
    }
    usable_count_ = 0;
    for (usize i = 0; i < count; ++i)
    {
        if (usable_count_ > 0)
        {
            auto &prev = usable_[usable_count_ - 1];
            const uptr prev_end = prev.base + prev.size;
            if (prev_end != 0 && work[i].base <= prev_end)
            {
                const uptr cur_end = work[i].base + work[i].size;
                if (cur_end == 0 || cur_end > prev_end)
                    prev.size = cur_end == 0 ? static_cast<usize>(0 - prev.base) : static_cast<usize>(cur_end - prev.base);
                continue;
            }
        }
        usable_[usable_count_++] = work[i];
    }
    normalized_ = true;
    return Status::success();
}

usize PhysicalMemoryMap::usable_bytes() const
{
    usize total = 0;
    for (usize i = 0; i < usable_count_; ++i)
        total += usable_[i].size;
    return total;
}

usize PhysicalMemoryMap::usable_pages(usize page_size) const
{
    return page_size ? usable_bytes() / page_size : 0;
}

bool PhysicalMemoryMap::product_grade() const
{
    if (!normalized_ || usable_count_ == 0)
        return false;
    for (usize i = 0; i < input_count_; ++i)
        if (inputs_[i].region.type == RegionType::usable && inputs_[i].source == MapSource::synthetic)
            return false;
    return true;
}

} // namespace ok::memory
