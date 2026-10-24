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

} // namespace ok::boot
