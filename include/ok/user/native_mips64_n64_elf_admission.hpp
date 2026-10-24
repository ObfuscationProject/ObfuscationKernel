#pragma once

#include <ok/core/types.hpp>
#include <span>

namespace ok::user::mips64_n64_elf_admission
{

// Private n64 profile: this checks declared metadata, not code, mappings,
// instruction compatibility, CPU capability or signal resume provenance.
enum class Error : u8
{
    none,
    prefix_exceeds_file,
    header_outside_file,
    header_outside_prefix,
    invalid_ident,
    unsupported_osabi,
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
    // Nonzero only for a prefix shortage. The caller must apply its allocation
    // cap before converting to usize or reading a larger prefix.
    u64 required_prefix_bytes{};

    [[nodiscard]] constexpr bool ok() const { return error == Error::none; }
    [[nodiscard]] constexpr explicit operator bool() const { return ok(); }
};

namespace detail
{
[[nodiscard]] constexpr bool contains(u64 extent, u64 offset, u64 bytes)
{
    return offset <= extent && bytes <= extent - offset;
}

// A containing prefix range is proved before every call and before converting
// any ELF64 file offset to a span index, including on a narrower host.
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
[[nodiscard]] constexpr u64 be64(std::span<const std::byte> data, usize offset)
{
    return (u64{be32(data, offset)} << 32) | be32(data, offset + 4);
}
} // namespace detail

[[nodiscard]] constexpr Result admit(std::span<const std::byte> prefix, u64 file_bytes)
{
    using namespace detail;
    constexpr u64 header_bytes = 64;
    constexpr u64 program_bytes = 56;
    constexpr u64 abi_bytes = 24;
    constexpr u32 pt_mips_abiflags = 0x70000003;

    if (prefix.size() > file_bytes) return {Error::prefix_exceeds_file};
    if (file_bytes < header_bytes) return {Error::header_outside_file};
    if (prefix.size() < header_bytes) return {Error::header_outside_prefix, header_bytes};
    if (byte(prefix, 0) != 0x7f || byte(prefix, 1) != 'E' || byte(prefix, 2) != 'L' ||
        byte(prefix, 3) != 'F' || byte(prefix, 4) != 2 || byte(prefix, 5) != 2 ||
        byte(prefix, 6) != 1)
        return {Error::invalid_ident};
    const auto osabi = byte(prefix, 7);
    const auto abi_version = byte(prefix, 8);
    if (!((osabi == 0 && abi_version == 0) || (osabi == 3 && abi_version == 1)))
        return {Error::unsupported_osabi};
    if (be16(prefix, 16) != 2) return {Error::unsupported_type};
    if (be16(prefix, 18) != 8) return {Error::unsupported_machine};
    if (be32(prefix, 20) != 1) return {Error::unsupported_version};
    if (be16(prefix, 52) != header_bytes) return {Error::unsupported_header_size};
    // EF_MIPS_ARCH_64R2, n64 (no ABI2/O32 flags). NOREORDER/PIC/CPIC may vary.
    // This excludes NAN2008, MIPS16, microMIPS, MDMX and alternate ISA profiles.
    if ((be32(prefix, 48) & ~u32{7}) != 0x80000000)
        return {Error::unsupported_flags};
    if (be16(prefix, 54) != program_bytes) return {Error::unsupported_program_entry_size};
    const u64 count = be16(prefix, 56);
    // Extended numbering requires a separately checked section-table path.
    if (count == 0 || count == 0xffff) return {Error::unsupported_program_count};
    const u64 table = be64(prefix, 32);
    if (table < header_bytes) return {Error::overlapping_program_table};
    const u64 table_bytes = count * program_bytes; // count <= 65534.
    if (!contains(file_bytes, table, table_bytes)) return {Error::program_table_outside_file};
    if (!contains(prefix.size(), table, table_bytes))
        return {Error::program_table_outside_prefix, table + table_bytes};

    bool found = false;
    u64 record = 0;
    for (u64 index = 0; index < count; ++index)
    {
        const auto ph = static_cast<usize>(table + index * program_bytes);
        const u32 type = be32(prefix, ph);
        if (type == 2 || type == 3) return {Error::dynamic_image};
        if (type == 0) continue; // PT_NULL has no defined payload fields.
        const u64 offset = be64(prefix, ph + 8);
        const u64 bytes = be64(prefix, ph + 32);
        if (!contains(file_bytes, offset, bytes)) return {Error::segment_outside_file};
        if (type != pt_mips_abiflags) continue;
        if (found) return {Error::duplicate_abi_flags};
        found = true;
        if (bytes != abi_bytes || be64(prefix, ph + 40) != abi_bytes)
            return {Error::unsupported_abi_record_size};
        record = offset;
    }
    if (!found) return {Error::missing_abi_flags};
    // Complete-table scan prevents a later dynamic segment or duplicate from
    // being hidden behind a prefix request for the first ABI record.
    if (!contains(prefix.size(), record, abi_bytes))
        return {Error::abi_record_outside_prefix, record + abi_bytes};
    const auto at = static_cast<usize>(record);
    if (be16(prefix, at) != 0) return {Error::unsupported_abi_version};
    if (byte(prefix, at + 2) != 64 || byte(prefix, at + 3) != 2)
        return {Error::unsupported_isa};
    if (byte(prefix, at + 4) != 2) return {Error::unsupported_gpr_size};
    if (byte(prefix, at + 5) != 2) return {Error::unsupported_cpr1_size};
    if (byte(prefix, at + 6) != 0) return {Error::unsupported_cpr2_size};
    if (byte(prefix, at + 7) != 1) return {Error::unsupported_fp_abi};
    if (be32(prefix, at + 8) != 0) return {Error::unsupported_isa_extension};
    if (be32(prefix, at + 12) != 0) return {Error::unsupported_ases};
    if ((be32(prefix, at + 16) & ~u32{1}) != 0) return {Error::unsupported_abi_flags1};
    if (be32(prefix, at + 20) != 0) return {Error::unsupported_abi_flags2};
    return {};
}

} // namespace ok::user::mips64_n64_elf_admission
