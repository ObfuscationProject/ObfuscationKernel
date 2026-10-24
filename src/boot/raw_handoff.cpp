#include "ok/boot/raw_handoff.hpp"

namespace
{
constinit ok::boot::RawBootHandoff primary_handoff;
constinit ok::boot::Arm32EntryState arm_entry;
}

extern "C" void ok_boot_capture(ok::uptr first, ok::uptr second, ok::uptr third, ok::uptr fourth) noexcept
{
    primary_handoff.arguments = {first, second, third, fourth};
    primary_handoff.captured = true;
}

extern "C" void ok_arm32_boot_capture(ok::u32 cpsr, ok::u32 sctlr, ok::u32 mpidr) noexcept
{
    arm_entry = {cpsr, sctlr, mpidr, true};
}

namespace ok::boot
{
const Arm32EntryState &arm32_entry_state() noexcept { return arm_entry; }
const RawBootHandoff &raw_boot_handoff() noexcept
{
    return primary_handoff;
}
} // namespace ok::boot
