#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <span>

namespace ok::boot
{

struct CpuTopologyMap;

inline constexpr usize max_acpi_table_bytes = 256 * 1024;

// Physical memory access for firmware table discovery. window returns exactly
// size readable bytes at physical, or an empty span when that range cannot be
// read; nothing outside a returned window is ever touched. Windows must stay
// valid until the parse call returns. Hosted tests supply synthetic memory, the
// kernel its boot identity map.
struct PhysicalReader
{
    using Window = std::span<const std::byte> (*)(void *context, u64 physical, usize size);
    Window window{};
    void *context{};
};

// One MADT I/O APIC: its registers and the first global system interrupt
// (GSI) its redirection entries carry.
struct IoApicDescription
{
    u32 id{};
    u64 address{};
    u32 gsi_base{};
};

// How a GSI is wired: an ISA interrupt source override's MPS INTI flags, with
// "conforms to the bus" already resolved (ISA: edge, active high).
struct GsiWiring
{
    u32 gsi{};
    bool level{};
    bool active_low{};
};

// The MADT's interrupt controller description for the x86 I/O APIC path. An
// ISA IRQ without an override is the GSI of the same number, edge-triggered
// and active high; a GSI no ISA IRQ reaches is a PCI line, level-triggered and
// active low.
struct MadtInterrupts
{
    static constexpr usize max_io_apics = 4;
    static constexpr usize isa_irqs = 16;

    std::array<IoApicDescription, max_io_apics> io_apics{};
    usize io_apic_count{};
    // Indexed by ISA IRQ; `overridden` marks the IRQs an override names.
    std::array<GsiWiring, isa_irqs> isa{};
    std::array<bool, isa_irqs> overridden{};
    bool pcat_compat{}; // The board also has the dual 8259 pair.
    bool truncated{};   // More I/O APICs than max_io_apics.

    [[nodiscard]] constexpr GsiWiring isa_irq(u8 irq) const
    {
        if (irq >= isa_irqs)
            return {irq, false, false};
        return overridden[irq] ? isa[irq] : GsiWiring{irq, false, false};
    }
    // The wiring of `gsi`: an ISA IRQ's that reaches it, otherwise PCI's.
    [[nodiscard]] constexpr GsiWiring gsi(u32 number) const
    {
        for (u8 irq = 0; irq < isa_irqs; ++irq)
            if (isa_irq(irq).gsi == number && (overridden[irq] || !reached_by_override(number)))
                return isa_irq(irq);
        return {number, true, true};
    }

  private:
    [[nodiscard]] constexpr bool reached_by_override(u32 number) const
    {
        for (u8 irq = 0; irq < isa_irqs; ++irq)
            if (overridden[irq] && isa[irq].gsi == number)
                return true;
        return false;
    }
};

// Scan the first KiB of the EBDA (real-mode segment at 0x40E) and then
// 0xE0000-0xFFFFF on 16-byte boundaries for an "RSD PTR " structure whose
// ACPI 1.0 checksum, and for revision 2+ its length and extended checksum, hold.
Status find_acpi_rsdp(PhysicalReader reader, u64 &rsdp);

// Follow the XSDT (revision 2+ with a nonzero address) or the RSDT to the MADT
// and record its usable local APIC and x2APIC entries. Enabled entries are
// present CPUs; online-capable but not enabled entries are recorded disabled;
// entries with neither flag are skipped. The boot CPU is unknown until the
// caller marks its own APIC id with mark_boot_cpu. With `interrupts`, also
// record the I/O APIC and ISA interrupt source override entries. Failure
// leaves both outputs unchanged; no table length is trusted beyond the
// reader's window.
Status parse_acpi_madt(PhysicalReader reader, u64 rsdp, CpuTopologyMap &output, MadtInterrupts *interrupts = nullptr);

} // namespace ok::boot
