#pragma once
#include "ok/boot/qemu_runtime_frames.hpp"
namespace ok::test
{
// Bootstrap bookkeeping only. The addresses below are never dereferenced.
inline Status check_qualified_runtime_frames()
{
    using namespace boot;
    static FirmwareMemory source;
    static QualifiedBootMemory scratch, qualified;
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<1024, 4> metadata;
    source = {};
    source.source = {arch::Architecture::i386, BootProtocol::bios_e820, FirmwareFormat::e820};
    source.source_storage = {0x5000, 3088};
    source.e820.count = 1;
    source.e820.records[0] = {0, 0x100000000ULL, 1, 1};
    QemuEntryMemoryPlan plan;
    const std::array extra{LoadedReservation{{0x180000, 4096}, true}};
    if (auto status = qemu_runtime_memory_plan<arch::Architecture::i386>(
            source, {0x100000, 0x200000}, extra, 1024, plan); !status)
        return status;
    if (plan.policy.accessible.size != 0x40000000)
        return Status::fault("runtime frame plan kept an inaccessible entry aperture");
    if (auto status = qualify_boot_memory(source, {plan.reservations.begin(), plan.reservations.size()},
                                          plan.policy, scratch, qualified); !status)
        return status;
    if (auto status = initialize_qualified_frames(qualified, frames, metadata.bytes); !status)
        return status;
    auto first = frames.allocate();
    if (!first || first.value().address != 0x500000 || frames.free_frames() != 1023)
        return Status::fault("qualified frame allocation crossed a loaded reservation");
    const auto reinitialized = initialize_qualified_frames(qualified, frames, metadata.bytes);
    const auto released = frames.release(first.value());
    if (reinitialized.code() != StatusCode::busy || !released || frames.free_frames() != 1024)
        return Status::fault("qualified frame bridge lost a live owner");
    if (frames.release(first.value()))
        return Status::fault("qualified frame bridge accepted stale ownership");
    return Status::success();
}
} // namespace ok::test
