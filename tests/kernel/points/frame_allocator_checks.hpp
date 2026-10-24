#pragma once

#include "ok/memory/memory.hpp"

namespace ok::test
{

// Shared allocation bookkeeping checks; these do not install hardware mappings.
inline Status check_frame_allocator()
{
    static memory::FrameAllocator frames;
    const std::array regions{
        memory::MemoryRegion{0x1000'0000, 0x2000, memory::RegionType::usable},
        memory::MemoryRegion{0x3001, 0x1000, memory::RegionType::reserved},
        memory::MemoryRegion{0x1001, 0x7fff, memory::RegionType::usable},
        memory::MemoryRegion{0x5000, 0x1000, memory::RegionType::usable},
        memory::MemoryRegion{0x7000, 0x1000, memory::RegionType::kernel_image},
        memory::MemoryRegion{0x8000, 1, memory::RegionType::device_mmio},
    };
    if (auto status = frames.initialize(regions, 4096); !status.ok())
    {
        return status;
    }
    if (frames.total_frames() != 5 || frames.free_frames() != 5)
    {
        return Status::fault("physical frame capacity ignores holes or reservations");
    }
    constexpr std::array<uptr, 5> expected{0x2000, 0x5000, 0x6000, 0x1000'0000, 0x1000'1000};
    std::array<memory::PhysicalFrame, expected.size()> owned{};
    for (usize i = 0; i < expected.size(); ++i)
    {
        auto frame = frames.allocate();
        if (!frame || frame.value().address != expected[i] || frames.free_frames() != expected.size() - i - 1)
        {
            return Status::fault("physical frame allocation escaped usable regions");
        }
        owned[i] = frame.value();
    }
    if (frames.allocate().status().code() != StatusCode::no_memory ||
        frames.initialize(regions, 4096).code() != StatusCode::busy)
    {
        return Status::fault("physical frame exhaustion or live reset was accepted");
    }
    auto forged = owned[0];
    forged.address = 0x3000;
    if (frames.release(forged).ok())
    {
        return Status::fault("physical frame release accepted a forged address");
    }
    forged = owned[0];
    forged.owner = nullptr;
    if (frames.release(forged).ok() || frames.free_frames() != 0)
    {
        return Status::fault("physical frame release accepted a foreign owner");
    }
    for (auto frame : owned)
    {
        if (auto status = frames.release(frame); !status.ok())
        {
            return status;
        }
    }
    if (frames.release(owned[0]).ok())
    {
        return Status::fault("physical frame was released twice");
    }
    auto reused = frames.allocate();
    if (!reused || reused.value().address != owned[0].address || frames.release(owned[0]).ok() ||
        frames.free_frames() != expected.size() - 1)
    {
        return Status::fault("stale physical frame handle released a reused page");
    }
    if (auto status = frames.release(reused.value()); !status.ok())
    {
        return status;
    }
    if (auto status = frames.initialize(regions, 4096); !status.ok())
    {
        return status;
    }
    auto reset_frame = frames.allocate();
    if (!reset_frame || frames.release(reused.value()).ok())
    {
        return Status::fault("allocator reset revived a stale frame handle");
    }
    if (auto status = frames.release(reset_frame.value()); !status.ok())
    {
        return status;
    }

    const std::array overflow{memory::MemoryRegion{~uptr{0} - 7, 16, memory::RegionType::usable}};
    if (frames.initialize(overflow, 4096).code() != StatusCode::overflow ||
        frames.initialize(regions, 0).code() != StatusCode::invalid_argument ||
        frames.initialize(regions, 3000).code() != StatusCode::invalid_argument ||
        frames.total_frames() != 5 || frames.free_frames() != 5)
    {
        return Status::fault("invalid physical map changed the active allocator");
    }
    const std::array too_large{memory::MemoryRegion{
        0x1000, (memory::max_physical_frames + 1) * 4096, memory::RegionType::usable}};
    if (frames.initialize(too_large, 4096).code() != StatusCode::overflow || frames.total_frames() != 5)
    {
        return Status::fault("physical map was silently truncated to bootstrap capacity");
    }
    return Status::success();
}

} // namespace ok::test
