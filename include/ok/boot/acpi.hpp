#pragma once

#include "ok/core/types.hpp"

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

// Scan the first KiB of the EBDA (real-mode segment at 0x40E) and then
// 0xE0000-0xFFFFF on 16-byte boundaries for an "RSD PTR " structure whose
// ACPI 1.0 checksum, and for revision 2+ its length and extended checksum, hold.
Status find_acpi_rsdp(PhysicalReader reader, u64 &rsdp);

// Follow the XSDT (revision 2+ with a nonzero address) or the RSDT to the MADT
// and record its usable local APIC and x2APIC entries. Enabled entries are
// present CPUs; online-capable but not enabled entries are recorded disabled;
// entries with neither flag are skipped. The boot CPU is unknown until the
// caller marks its own APIC id with mark_boot_cpu. Failure leaves output
// unchanged; no table length is trusted beyond the reader's window.
Status parse_acpi_madt(PhysicalReader reader, u64 rsdp, CpuTopologyMap &output);

} // namespace ok::boot
