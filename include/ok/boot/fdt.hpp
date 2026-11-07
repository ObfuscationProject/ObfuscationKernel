#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <span>

namespace ok::boot
{

inline constexpr usize max_fdt_bytes = 2 * 1024 * 1024;
inline constexpr usize max_fdt_depth = 32;
inline constexpr usize max_fdt_ram_ranges = 64;
inline constexpr usize max_fdt_reservations = 128;

struct PhysicalRange
{
    u64 address{};
    u64 size{};
};

enum class ReservationSource : u8
{
    reserve_table,
    reserved_memory,
};

struct FirmwareReservation
{
    PhysicalRange range{};
    ReservationSource source{};
    bool no_map{};
    bool reusable{};
};

struct FdtMemoryMap
{
    std::array<PhysicalRange, max_fdt_ram_ranges> ram{};
    std::array<FirmwareReservation, max_fdt_reservations> reservations{};
    usize ram_count{};
    usize reservation_count{};
    u32 blob_size{};
    u32 boot_cpu_id{};
};

// Parse a complete, already mapped FDT v17 blob. Success copies exact firmware
// records; failure leaves output unchanged. No pointers into blob are retained.
// The caller must additionally reserve the blob, kernel, stacks, boot archive
// and page tables, and qualify addresses against the architecture/platform.
Status parse_fdt_memory(std::span<const std::byte> blob, FdtMemoryMap &output);

struct CpuTopologyMap;

// Which binding the /cpus children follow: arm reg is an MPIDR affinity value
// started through PSCI; riscv reg is a hart id released through CLINT MSIP.
enum class FdtCpuFlavor : u8
{
    arm,
    riscv,
};

// Parse /cpus, /psci and timebase-frequency of a complete FDT v17 blob into
// ok/boot/cpu_topology.hpp's map. Success describes the CPUs and either a start
// method or the reason the platform must run UP; the boot CPU is the header's
// boot_cpuid_phys when it names an enabled CPU (otherwise the caller marks it).
// Failure leaves output unchanged. Nothing is started and no pointer is retained.
// Without a cpu_on property a PSCI 0.2+ node gets the SMC64 CPU_ON id
// 0xC4000003; an AArch32 caller must use 0x84000003 instead.
Status parse_fdt_cpus(std::span<const std::byte> blob, FdtCpuFlavor flavor, CpuTopologyMap &output);

} // namespace ok::boot
