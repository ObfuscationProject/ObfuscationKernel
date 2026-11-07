#pragma once

#include "ok/core/types.hpp"

// The xAPIC register page. boot.S maps its 2 MiB uncached at the same virtual
// address, and user spaces inherit that supervisor mapping, so the trap path
// acknowledges on any translation. SMP bring-up enables the boot CPU's local
// APIC for IPIs; the interrupt service enables it on every boot that switches
// to the I/O APIC (ioapic.hpp), and other boots keep the 8259 and PIT alone.
namespace ok::arch::x86::lapic
{

inline constexpr uptr base = 0xfee00000;
inline constexpr u32 timer_vector = 0xef;
inline constexpr u32 ipi_vector = 0xf0;
inline constexpr u32 spurious_vector = 0xff;

enum Register : u32
{
    id = 0x20,
    task_priority = 0x80,
    eoi = 0xb0,
    request_32_63 = 0x210, // IRR bits for vectors 32..63.
    spurious = 0xf0,
    icr_low = 0x300,
    icr_high = 0x310,
    lvt_timer = 0x320,
    lvt_lint0 = 0x350,
    lvt_lint1 = 0x360,
    lvt_error = 0x370,
    timer_initial = 0x380,
    timer_current = 0x390,
    timer_divide = 0x3e0,
};

inline constexpr u32 lvt_masked = 1u << 16;
inline constexpr u32 delivery_extint = 0x700;
inline constexpr u32 delivery_nmi = 0x400;
inline constexpr u32 software_enable = 0x100;
inline constexpr u32 timer_periodic = 1u << 17;
inline constexpr u32 delivery_pending = 1u << 12;

inline volatile u32 &reg(Register offset) noexcept
{
    return *reinterpret_cast<volatile u32 *>(base + offset);
}

// Not for spurious interrupts, which set no in-service bit.
inline void end_of_interrupt() noexcept
{
    reg(eoi) = 0;
}

// Counts of the divide-by-16 timer per 10 ms, measured once on the boot CPU;
// every secondary runs its periodic local tick from it.
inline constinit u32 timer_period{};

} // namespace ok::arch::x86::lapic
