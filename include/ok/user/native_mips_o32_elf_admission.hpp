#pragma once

#include <ok/core/types.hpp>
#include <span>

namespace ok::user::mips_o32_elf_admission
{

// This private profile checks declared ELF metadata, not executed instructions,
// mappings, Linux personality, CPU capability or signal resume provenance.
enum class Error : u8
{
    none,
    prefix_exceeds_file,
    header_outside_file,
    header_outside_prefix,
    invalid_ident,
    unsupported_type,
    unsupported_machine,
    unsupported_version,
    unsupported_header_size,
    unsupported_flags,
    unsupported_program_entry_size,
    unsupported_program_count,
    overlapping_program_table,
    program_table_outside_file,
    program_table_outside_prefix,
    dynamic_image,
    segment_outside_file,
    missing_abi_flags,
    duplicate_abi_flags,
    unsupported_abi_record_size,
    abi_record_outside_prefix,
    unsupported_abi_version,
    unsupported_isa,
    unsupported_gpr_size,
    unsupported_cpr1_size,
    unsupported_cpr2_size,
    unsupported_fp_abi,
    unsupported_isa_extension,
    unsupported_ases,
    unsupported_abi_flags1,
    unsupported_abi_flags2,
};

struct Result final
{
    Error error{Error::none};
    // Nonzero only for a prefix shortage. This is the next checked prefix size,
    // not permission to allocate that much memory; the caller applies its cap.
    usize required_prefix_bytes{};

    [[nodiscard]] constexpr bool ok() const { return error == Error::none; }
    [[nodiscard]] constexpr explicit operator bool() const { return ok(); }
};

namespace detail
{
[[nodiscard]] constexpr bool contains(usize extent, usize offset, usize bytes)
{
    return offset <= extent && bytes <= extent - offset;
}

// Callers prove each containing header/record before reading any field.
[[nodiscard]] constexpr u8 byte(std::span<const std::byte> data, usize offset)
{
    return std::to_integer<u8>(data[offset]);
}
[[nodiscard]] constexpr u16 be16(std::span<const std::byte> data, usize offset)
{
    return static_cast<u16>((u16{byte(data, offset)} << 8) | byte(data, offset + 1));
}
[[nodiscard]] constexpr u32 be32(std::span<const std::byte> data, usize offset)
{
    return (u32{byte(data, offset)} << 24) | (u32{byte(data, offset + 1)} << 16) |
           (u32{byte(data, offset + 2)} << 8) | byte(data, offset + 3);
}
} // namespace detail

[[nodiscard]] constexpr Result admit(std::span<const std::byte> prefix, usize file_bytes)
{
    using namespace detail;
    constexpr usize header_bytes = 52;
    constexpr usize program_bytes = 32;
    constexpr usize abi_bytes = 24;
    constexpr u32 pt_mips_abiflags = 0x70000003;

    if (prefix.size() > file_bytes) return {Error::prefix_exceeds_file};
    if (file_bytes < header_bytes) return {Error::header_outside_file};
    if (prefix.size() < header_bytes) return {Error::header_outside_prefix, header_bytes};
    if (byte(prefix, 0) != 0x7f || byte(prefix, 1) != 'E' || byte(prefix, 2) != 'L' ||
        byte(prefix, 3) != 'F' || byte(prefix, 4) != 1 || byte(prefix, 5) != 2 ||
        byte(prefix, 6) != 1)
        return {Error::invalid_ident};
    if (be16(prefix, 16) != 2) return {Error::unsupported_type};
    if (be16(prefix, 18) != 8) return {Error::unsupported_machine};
    if (be32(prefix, 20) != 1) return {Error::unsupported_version};
    if (be16(prefix, 40) != header_bytes) return {Error::unsupported_header_size};
    // EF_MIPS_ARCH_32R2 | EF_MIPS_ABI_O32; only NOREORDER/PIC/CPIC may vary.
    if ((be32(prefix, 36) & ~u32{7}) != 0x70001000)
        return {Error::unsupported_flags};
    if (be16(prefix, 42) != program_bytes) return {Error::unsupported_program_entry_size};
    const usize count = be16(prefix, 44);
    // Extended numbering would require a separately checked section-table path.
    if (count == 0 || count == 0xffff) return {Error::unsupported_program_count};
    const usize table = be32(prefix, 28);
    if (table < header_bytes) return {Error::overlapping_program_table};
    // count <= 65534, so the multiplication fits both native o32 and host usize.
    const usize table_bytes = count * program_bytes;
    if (!contains(file_bytes, table, table_bytes)) return {Error::program_table_outside_file};
    if (!contains(prefix.size(), table, table_bytes))
        return {Error::program_table_outside_prefix, table + table_bytes};

    bool found = false;
    usize record = 0;
    for (usize index = 0; index < count; ++index)
    {
        const usize ph = table + index * program_bytes;
        const u32 type = be32(prefix, ph);
        // Reject both an interpreter and a dynamic table, including zero sizes.
        if (type == 2 || type == 3) return {Error::dynamic_image};
        if (type == 0) continue; // PT_NULL has no defined payload fields.
        const usize offset = be32(prefix, ph + 4);
        const usize bytes = be32(prefix, ph + 16);
        if (!contains(file_bytes, offset, bytes)) return {Error::segment_outside_file};
        if (type != pt_mips_abiflags) continue;
        if (found) return {Error::duplicate_abi_flags};
        found = true;
        if (bytes != abi_bytes || be32(prefix, ph + 20) != abi_bytes)
            return {Error::unsupported_abi_record_size};
        record = offset;
    }
    if (!found) return {Error::missing_abi_flags};
    // Scan the complete table before requesting the payload; a hidden duplicate
    // or dynamic segment must never be ignored after seeing the first record.
    if (!contains(prefix.size(), record, abi_bytes))
        return {Error::abi_record_outside_prefix, record + abi_bytes};
    if (be16(prefix, record) != 0) return {Error::unsupported_abi_version};
    if (byte(prefix, record + 2) != 32 || byte(prefix, record + 3) != 2)
        return {Error::unsupported_isa};
    if (byte(prefix, record + 4) != 1) return {Error::unsupported_gpr_size};
    if (byte(prefix, record + 5) != 1) return {Error::unsupported_cpr1_size};
    if (byte(prefix, record + 6) != 0) return {Error::unsupported_cpr2_size};
    if (byte(prefix, record + 7) != 1) return {Error::unsupported_fp_abi};
    if (be32(prefix, record + 8) != 0) return {Error::unsupported_isa_extension};
    if (be32(prefix, record + 12) != 0) return {Error::unsupported_ases};
    if ((be32(prefix, record + 16) & ~u32{1}) != 0) return {Error::unsupported_abi_flags1};
    if (be32(prefix, record + 20) != 0) return {Error::unsupported_abi_flags2};
    return {};
}

} // namespace ok::user::mips_o32_elf_admission
