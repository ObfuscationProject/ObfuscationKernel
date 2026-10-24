#pragma once
#include "ok/boot/qemu_entry_memory.hpp"
#include "ok/memory/memory.hpp"

namespace ok::boot
{
// A bounded QEMU bootstrap bridge, serialized on the boot CPU. Execution backend
// initialization must succeed before the qualified pages may be dereferenced.
// It neither installs the process allocator nor grants SMP/DMA lifetime authority.
template <arch::Architecture A>
Status qemu_runtime_memory_plan(const FirmwareMemory &source, PhysicalRange linked_image,
                               std::span<const LoadedReservation> extras, u64 maximum_pages,
                               QemuEntryMemoryPlan &output)
{
    if (maximum_pages > memory::max_physical_frames)
        return Status::no_memory("runtime frame bookkeeping capacity exceeded");
    QemuEntryMemoryPlan candidate;
    if (auto status = qemu_entry_memory_plan<A>(source, linked_image, extras, maximum_pages, candidate); !status)
        return status;
    using enum arch::Architecture;
    // i386 enables PAE with only PDPT[0] populated. x86_64 retains the same
    // first-GiB supervisor aperture as boot. Never reuse the i386 entry 4GiB map.
    if constexpr (A == i386 || A == x86_64)
        candidate.policy.accessible = {0, 0x40000000};
    // ARM maps exactly root entries 1024..1279 as supervisor RAM.
    else if constexpr (A == arm32)
        candidate.policy.accessible = {0x40000000, 0x10000000};
    // PPC replaces the ePAPR TLB mapping with 256MiB supervisor RAM. Retain the
    // stricter captured entry extent when it is smaller.
    else if constexpr (A == ppc)
        if (candidate.policy.accessible.size > 0x10000000)
            candidate.policy.accessible.size = 0x10000000;
    // AArch64's 1GiB supervisor RAM covers the established 512MiB QEMU entry
    // window. RISC-V stays in M-mode and bypasses its userspace Sv32/Sv39 tables.
    candidate.policy.maximum_ranges = memory::max_physical_frame_ranges;
    if (qualified_detail::end(linked_image) > qualified_detail::end(candidate.policy.accessible))
        return Status::fault("complete image exceeds runtime supervisor aperture");
    output = candidate;
    return Status::success();
}

inline Status initialize_qualified_frames(const QualifiedBootMemory &qualified, memory::FrameAllocator &frames)
{
    if (!qualified.valid() || qualified.policy().page_size != memory::vm_test_page_size ||
        qualified.managed().empty() || qualified.managed().size() > memory::max_physical_frame_ranges ||
        qualified.page_count() > memory::max_physical_frames)
        return Status::invalid_argument("qualified memory exceeds frame allocator contract");
    StaticVector<memory::MemoryRegion, memory::max_physical_frame_ranges> regions;
    for (const auto range : qualified.managed())
    {
        // Do not truncate full-width firmware addresses into a 32-bit profile.
        if (range.address > ~uptr{0} || range.size > ~usize{0} ||
            range.size > ~uptr{0} - static_cast<uptr>(range.address))
            return Status::overflow("qualified range is not natively representable");
        if (auto status = regions.push_back({static_cast<uptr>(range.address), static_cast<usize>(range.size),
                                            memory::RegionType::usable}); !status)
            return status;
    }
    return frames.initialize({regions.begin(), regions.size()}, memory::vm_test_page_size);
}
} // namespace ok::boot
