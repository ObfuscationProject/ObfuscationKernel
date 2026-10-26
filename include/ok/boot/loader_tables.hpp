#pragma once

#include "ok/boot/fdt.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::boot
{

// Pure decoders for loader-provided tables that only point at, or summarize,
// the platform memory description. Callers own every physical read.

// 64-bit UEFI system table: "IBI SYST", NumberOfTableEntries at 0x68 and the
// ConfigurationTable pointer at 0x70. Entries are a GUID and a pointer.
inline constexpr u64 efi_system_table_signature = 0x5453595320494249ULL;
inline constexpr usize efi_system_table_bytes = 0x78;
inline constexpr usize efi_configuration_entry_bytes = 24;
inline constexpr usize max_efi_configuration_entries = 64;
// EFI_DTB_TABLE_GUID b1b621d5-f19c-41a5-830b-d9152c69aae0, in-memory byte order.
inline constexpr std::array<u8, 16> efi_device_tree_guid{0xd5, 0x21, 0xb6, 0xb1, 0x9c, 0xf1, 0xa5, 0x41,
                                                         0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0};

namespace loader_detail
{
inline u64 little64(std::span<const std::byte> bytes, usize offset)
{
    u64 value{};
    for (usize i = 0; i < 8; ++i)
        value |= std::to_integer<u64>(bytes[offset + i]) << (i * 8);
    return value;
}
inline u32 word32(std::span<const std::byte> bytes, usize offset, bool big_endian)
{
    u32 value{};
    for (usize i = 0; i < 4; ++i)
        value |= std::to_integer<u32>(bytes[offset + i]) << ((big_endian ? 3 - i : i) * 8);
    return value;
}
} // namespace loader_detail

struct EfiConfigurationTables
{
    u64 address{};
    u64 count{};
};

inline Result<EfiConfigurationTables> parse_efi_system_table(std::span<const std::byte> bytes)
{
    using namespace loader_detail;
    if (bytes.size() < efi_system_table_bytes)
        return Status::invalid_argument("truncated EFI system table");
    if (little64(bytes, 0) != efi_system_table_signature)
        return Status::invalid_argument("EFI system table signature invalid");
    const EfiConfigurationTables tables{little64(bytes, 0x70), little64(bytes, 0x68)};
    if (!tables.count || tables.count > max_efi_configuration_entries || !tables.address || (tables.address & 7) ||
        tables.address > ~u64{0} - tables.count * efi_configuration_entry_bytes)
        return Status::invalid_argument("EFI configuration table extent invalid");
    return tables;
}

// Exactly one device-tree entry is accepted; a duplicate is ambiguous.
inline Result<u64> find_efi_device_tree(std::span<const std::byte> table, u64 count)
{
    if (count > max_efi_configuration_entries || table.size() < count * efi_configuration_entry_bytes)
        return Status::invalid_argument("truncated EFI configuration table");
    u64 found{};
    for (usize entry = 0; entry < count; ++entry)
    {
        const auto *record = table.data() + entry * efi_configuration_entry_bytes;
        bool match = true;
        for (usize i = 0; i < efi_device_tree_guid.size(); ++i)
            match &= std::to_integer<u8>(record[i]) == efi_device_tree_guid[i];
        if (!match)
            continue;
        if (found)
            return Status::invalid_argument("EFI configuration table names two device trees");
        found = loader_detail::little64(table, entry * efi_configuration_entry_bytes + 16);
        if (!found)
            return Status::invalid_argument("EFI device tree pointer is null");
    }
    if (!found)
        return Status::not_found("EFI configuration table has no device tree");
    return found;
}

// MIPS Malta (YAMON-style) PROM environment: argv/envp tables of 32-bit KSEG0
// pointers, envp a null-terminated name/value pointer list, decimal or 0x
// values. QEMU and YAMON report low RAM ("memsize") and total RAM ("ememsize").
inline constexpr usize max_malta_prom_pointers = 32;
inline constexpr usize max_malta_prom_string = 256;
inline constexpr u64 malta_kseg0 = 0x80000000;
inline constexpr u64 malta_kseg_size = 0x20000000;
inline constexpr u64 malta_low_ram_limit = 0x10000000;
inline constexpr u64 malta_high_ram_base = 0x90000000;

// Physical address of a KSEG0 pointer (a 64-bit register sign-extends it).
inline Result<u64> malta_kseg0_physical(u64 pointer)
{
    if ((pointer >> 32) == 0xffffffffULL)
        pointer &= 0xffffffffULL;
    if (pointer < malta_kseg0 || pointer >= malta_kseg0 + malta_kseg_size)
        return Status::invalid_argument("PROM pointer is not a KSEG0 address");
    return pointer - malta_kseg0;
}

inline Result<u64> parse_malta_prom_number(std::string_view text)
{
    u64 base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 20)
        return Status::invalid_argument("PROM number is empty or too long");
    u64 value{};
    for (const char c : text)
    {
        u64 digit{};
        if (c >= '0' && c <= '9')
            digit = static_cast<u64>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = static_cast<u64>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = static_cast<u64>(c - 'A' + 10);
        else
            return Status::invalid_argument("PROM number has an invalid digit");
        if (value > (~u64{0} - digit) / base)
            return Status::overflow("PROM number overflows");
        value = value * base + digit;
    }
    return value;
}

} // namespace ok::boot
