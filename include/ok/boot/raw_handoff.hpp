#pragma once

#include "ok/core/types.hpp"

#include <array>

namespace ok::boot
{

// Owned primary-CPU entry arguments. Assembly calls the capture hook only after
// clearing BSS and establishing the target C ABI. This is not a memory map.
// x86: EAX, EBX, 0, 0; PPC: r3, r6, r7, 0; other profiles: first four argument
// registers. Zero words remain unknown inputs, never synthetic firmware data.
struct RawBootHandoff
{
    std::array<uptr, 4> arguments{};
    bool captured{};
};

// Actual ARM loader state, captured by boot assembly before changing masks.
// The Linux-header contract requires boot CPU 0, ARM SVC, MMU/cache disabled.
struct Arm32EntryState
{
    u32 cpsr{}, sctlr{}, mpidr{};
    bool captured{};
};
[[nodiscard]] const Arm32EntryState &arm32_entry_state() noexcept;
[[nodiscard]] inline bool valid_arm32_entry(const Arm32EntryState &state)
{
    return state.captured && (state.cpsr & 0x23f) == 0x13 &&
           (state.sctlr & 0x02000005) == 0 && (state.mpidr & 0x00ffffff) == 0;
}

// Capture completes before any secondary CPU or consumer can access this view.
[[nodiscard]] const RawBootHandoff &raw_boot_handoff() noexcept;

} // namespace ok::boot

extern "C" void ok_boot_capture(ok::uptr first, ok::uptr second, ok::uptr third, ok::uptr fourth) noexcept;

extern "C" void ok_arm32_boot_capture(ok::u32 cpsr, ok::u32 sctlr, ok::u32 mpidr) noexcept;
