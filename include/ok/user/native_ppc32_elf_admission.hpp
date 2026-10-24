#pragma once

#include <ok/core/types.hpp>
#include <span>

namespace ok::user::ppc32_elf_admission
{

enum class Personality : u8 { native_os, linux_user };
enum class RuntimeProfile : u8 { e500v2_scalar_soft_float };

// Declared metadata only. The explicit profile is a caller-selected contract,
// not a CPU measurement or a proof that the file contains only supported ISA.
enum class Error : u8
{
    none, unsupported_personality, unsupported_profile,
    prefix_exceeds_file, header_outside_file, header_outside_prefix,
    invalid_ident, unsupported_osabi, unsupported_type, unsupported_machine,
    unsupported_version, unsupported_header_size, unsupported_flags,
    unsupported_program_entry_size, unsupported_program_count,
    overlapping_program_table, program_table_outside_file, program_table_outside_prefix,
    dynamic_image, segment_outside_file,
    unsupported_section_entry_size, unsupported_section_count, unsupported_section_names,
    overlapping_section_table, section_table_outside_file, section_table_outside_prefix,
    invalid_null_section, section_outside_file, invalid_section_names,
    duplicate_attributes, invalid_attribute_section, attributes_outside_prefix,
    invalid_attribute_format, invalid_vendor_record, unsupported_vendor,
    duplicate_vendor, invalid_file_record, unsupported_attribute_scope,
    duplicate_file_record, invalid_uleb, unknown_attribute, duplicate_attribute,
    unsupported_fp_abi, unsupported_vector_abi,
};

struct Result final
{
    Error error{Error::none};
    // Nonzero only for prefix shortage. The caller must cap this u64 value
    // before allocating, reading or converting it to native usize.
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
[[nodiscard]] constexpr bool overlap(u64 a, u64 an, u64 b, u64 bn)
{
    // All table endpoints have been checked against their containing file.
    return a < b + bn && b < a + an;
}
[[nodiscard]] constexpr u8 byte(std::span<const std::byte> data, usize at)
{
    return std::to_integer<u8>(data[at]);
}
[[nodiscard]] constexpr u16 be16(std::span<const std::byte> data, usize at)
{
    return static_cast<u16>((u16{byte(data, at)} << 8) | byte(data, at + 1));
}
[[nodiscard]] constexpr u32 be32(std::span<const std::byte> data, usize at)
{
    return (u32{byte(data, at)} << 24) | (u32{byte(data, at + 1)} << 16) |
           (u32{byte(data, at + 2)} << 8) | byte(data, at + 3);
}

// Strict canonical unsigned 32-bit LEB128, bounded by the current record.
[[nodiscard]] constexpr bool uleb(std::span<const std::byte> data, usize &at,
                                usize end, u32 &value)
{
    value = 0;
    for (unsigned i = 0; i != 5; ++i)
    {
        if (at == end) return false;
        const u8 b = byte(data, at++);
        if (i == 4 && (b & 0xf0) != 0) return false;
        value |= u32{b & 0x7fU} << (7 * i);
        if ((b & 0x80) == 0) return i == 0 || (b & 0x7f) != 0;
    }
    return false;
}

[[nodiscard]] constexpr Result attributes(std::span<const std::byte> data)
{
    if (data.empty() || byte(data, 0) != 'A') return {Error::invalid_attribute_format};
    usize at = 1;
    bool vendor_seen = false;
    while (at < data.size())
    {
        const usize vendor_start = at;
        if (!contains(data.size(), at, 4)) return {Error::invalid_vendor_record};
        const u32 bytes = be32(data, at);
        if (bytes < 9 || !contains(data.size(), at, bytes))
            return {Error::invalid_vendor_record};
        const usize vendor_end = at + bytes;
        at += 4;
        const usize name_start = at;
        while (at < vendor_end && byte(data, at) != 0) ++at;
        if (at == vendor_end) return {Error::invalid_vendor_record};
        if (at - name_start != 3 || byte(data, name_start) != 'g' ||
            byte(data, name_start + 1) != 'n' || byte(data, name_start + 2) != 'u')
            return {Error::unsupported_vendor};
        if (vendor_seen) return {Error::duplicate_vendor};
        vendor_seen = true;
        ++at;
        bool file_seen = false;
        while (at < vendor_end)
        {
            const usize record_start = at;
            u32 scope = 0;
            if (!uleb(data, at, vendor_end, scope)) return {Error::invalid_uleb};
            if (scope != 1) return {Error::unsupported_attribute_scope};
            if (!contains(vendor_end, at, 4)) return {Error::invalid_file_record};
            const u32 record_bytes = be32(data, at);
            at += 4;
            if (record_bytes < at - record_start ||
                !contains(vendor_end, record_start, record_bytes))
                return {Error::invalid_file_record};
            if (file_seen) return {Error::duplicate_file_record};
            file_seen = true;
            const usize record_end = record_start + record_bytes;
            bool fp_seen = false, vector_seen = false;
            while (at < record_end)
            {
                u32 tag = 0, value = 0;
                if (!uleb(data, at, record_end, tag)) return {Error::invalid_uleb};
                if (tag != 4 && tag != 8) return {Error::unknown_attribute};
                if (!uleb(data, at, record_end, value)) return {Error::invalid_uleb};
                if (tag == 4)
                {
                    if (fp_seen) return {Error::duplicate_attribute};
                    fp_seen = true;
                    // GNU Power FP ABI: soft float; unspecified or 64-bit long double.
                    if (value != 2 && value != 10) return {Error::unsupported_fp_abi};
                }
                else
                {
                    if (vector_seen) return {Error::duplicate_attribute};
                    vector_seen = true;
                    // Generic vector calling convention, not AltiVec or SPE ABI.
                    if (value != 1) return {Error::unsupported_vector_abi};
                }
            }
        }
        if (!file_seen) return {Error::invalid_file_record};
        at = vendor_start + bytes;
    }
    return vendor_seen ? Result{} : Result{Error::invalid_vendor_record};
}
} // namespace detail

[[nodiscard]] constexpr Result admit(std::span<const std::byte> prefix, u64 file_bytes,
                                     Personality personality, RuntimeProfile profile)
{
    using namespace detail;
    if (personality != Personality::native_os && personality != Personality::linux_user)
        return {Error::unsupported_personality};
    if (profile != RuntimeProfile::e500v2_scalar_soft_float)
        return {Error::unsupported_profile};
    if (prefix.size() > file_bytes) return {Error::prefix_exceeds_file};
    if (file_bytes < 52) return {Error::header_outside_file};
    if (prefix.size() < 52) return {Error::header_outside_prefix, 52};
    if (byte(prefix, 0) != 0x7f || byte(prefix, 1) != 'E' || byte(prefix, 2) != 'L' ||
        byte(prefix, 3) != 'F' || byte(prefix, 4) != 1 || byte(prefix, 5) != 2 ||
        byte(prefix, 6) != 1) return {Error::invalid_ident};
    if (byte(prefix, 7) != (personality == Personality::native_os ? 0 : 3) ||
        byte(prefix, 8) != 0) return {Error::unsupported_osabi};
    for (usize i = 9; i != 16; ++i)
        if (byte(prefix, i) != 0) return {Error::invalid_ident};
    if (be16(prefix, 16) != 2) return {Error::unsupported_type};
    if (be16(prefix, 18) != 20) return {Error::unsupported_machine};
    if (be32(prefix, 20) != 1) return {Error::unsupported_version};
    if (be16(prefix, 40) != 52) return {Error::unsupported_header_size};
    if (be32(prefix, 36) != 0) return {Error::unsupported_flags};
    if (be16(prefix, 42) != 32) return {Error::unsupported_program_entry_size};
    const u64 ph_count = be16(prefix, 44);
    if (ph_count == 0 || ph_count == 0xffff) return {Error::unsupported_program_count};
    const u64 ph_at = be32(prefix, 28), ph_bytes = ph_count * 32;
    if (ph_at < 52) return {Error::overlapping_program_table};
    if (!contains(file_bytes, ph_at, ph_bytes)) return {Error::program_table_outside_file};
    if (!contains(prefix.size(), ph_at, ph_bytes))
        return {Error::program_table_outside_prefix, ph_at + ph_bytes};
    for (u64 i = 0; i != ph_count; ++i)
    {
        const auto ph = static_cast<usize>(ph_at + i * 32);
        const u32 type = be32(prefix, ph);
        if (type == 2 || type == 3) return {Error::dynamic_image};
        if (type == 0) continue;
        if (!contains(file_bytes, be32(prefix, ph + 4), be32(prefix, ph + 16)))
            return {Error::segment_outside_file};
    }

    const u64 sh_at = be32(prefix, 32), sh_count = be16(prefix, 48);
    const u16 sh_size = be16(prefix, 46), sh_names = be16(prefix, 50);
    if (sh_at == 0)
    {
        // ELF permits no section table; its absence supplies no ISA proof.
        if (sh_count != 0) return {Error::unsupported_section_count};
        if (sh_size != 0 && sh_size != 40) return {Error::unsupported_section_entry_size};
        if (sh_names != 0) return {Error::unsupported_section_names};
        return {};
    }
    if (sh_size != 40) return {Error::unsupported_section_entry_size};
    // Extended numbering and reserved-index ranges are outside this profile.
    if (sh_count == 0 || sh_count >= 0xff00) return {Error::unsupported_section_count};
    if (sh_names >= sh_count) return {Error::unsupported_section_names};
    const u64 sh_bytes = sh_count * 40;
    if (sh_at < 52) return {Error::overlapping_section_table};
    if (!contains(file_bytes, sh_at, sh_bytes)) return {Error::section_table_outside_file};
    if (overlap(sh_at, sh_bytes, ph_at, ph_bytes)) return {Error::overlapping_section_table};
    if (!contains(prefix.size(), sh_at, sh_bytes))
        return {Error::section_table_outside_prefix, sh_at + sh_bytes};
    // With extended numbering unsupported, the mandatory null header is zero.
    for (usize i = 0; i != 40; ++i)
        if (byte(prefix, static_cast<usize>(sh_at) + i) != 0)
            return {Error::invalid_null_section};
    bool found = false;
    u64 attribute_at = 0, attribute_bytes = 0;
    for (u64 i = 1; i != sh_count; ++i)
    {
        const auto sh = static_cast<usize>(sh_at + i * 40);
        const u32 type = be32(prefix, sh + 4);
        const u64 offset = be32(prefix, sh + 16), bytes = be32(prefix, sh + 20);
        if (type == 6) return {Error::dynamic_image};
        if (type != 0 && type != 8 && !contains(file_bytes, offset, bytes))
            return {Error::section_outside_file};
        if (i == sh_names && type != 3) return {Error::invalid_section_names};
        if (type != 0x6ffffff5) continue; // SHT_GNU_ATTRIBUTES, independent of names.
        if (found) return {Error::duplicate_attributes};
        found = true;
        if (be32(prefix, sh + 8) != 0 || be32(prefix, sh + 12) != 0 ||
            be32(prefix, sh + 24) != 0 || be32(prefix, sh + 28) != 0 ||
            be32(prefix, sh + 32) != 1 || be32(prefix, sh + 36) != 0 || bytes == 0 ||
            offset < 52 || overlap(offset, bytes, ph_at, ph_bytes) ||
            overlap(offset, bytes, sh_at, sh_bytes))
            return {Error::invalid_attribute_section};
        attribute_at = offset;
        attribute_bytes = bytes;
    }
    // Scan all section descriptors before requesting an attribute payload.
    if (!found) return {};
    if (!contains(prefix.size(), attribute_at, attribute_bytes))
        return {Error::attributes_outside_prefix, attribute_at + attribute_bytes};
    return attributes(prefix.subspan(static_cast<usize>(attribute_at),
                                     static_cast<usize>(attribute_bytes)));
}

} // namespace ok::user::ppc32_elf_admission
