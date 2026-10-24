#pragma once
#include "ok/core/types.hpp"
#include <array>
#include <span>
namespace ok::boot
{
inline constexpr u32 e820_handoff_magic = 0x30323845;
inline constexpr usize e820_record_capacity = 128;
inline constexpr usize e820_header_bytes = 16;
inline constexpr usize e820_record_bytes = 24;
struct E820Record
{
    u64 address{};
    u64 size{};
    u32 type{};
    u32 attributes{};
    bool usable() const { return type == 1 && size != 0 && (attributes & 1) != 0; }
};
struct E820Map
{
    std::array<E820Record, e820_record_capacity> records{};
    usize count{};
};
// Parse the owned table produced by the BIOS collector, not arbitrary firmware
// pointers. Preserve reservations/unknown types/attributes and overlapping RAM.
// The caller must reserve the table and every loaded object before allocation.
inline Status parse_e820_handoff(std::span<const std::byte> bytes, E820Map &output)
{
    auto little32 = [&](usize offset) {
        u32 result{};
        for (usize i = 0; i < 4; ++i) result |= u32(bytes[offset+i]) << (i*8);
        return result;
    };
    if (bytes.size() < e820_header_bytes)
        return Status::invalid_argument("truncated E820 header");
    if (little32(0) != e820_handoff_magic || little32(4) != 1 || little32(12) != e820_record_bytes)
        return Status::invalid_argument("E820 collection is absent, incomplete or unsupported");
    const u32 count = little32(8);
    if (count == 0 || count > e820_record_capacity || count > (bytes.size()-e820_header_bytes)/e820_record_bytes)
        return Status::invalid_argument("invalid E820 record count");
    E820Map candidate;
    for (u32 i = 0; i < count; ++i)
    {
        const usize offset = e820_header_bytes + i*e820_record_bytes;
        const u64 address = u64(little32(offset)) | (u64(little32(offset+4)) << 32);
        const u64 size = u64(little32(offset+8)) | (u64(little32(offset+12)) << 32);
        if (size > ~u64{0} - address)
            return Status::invalid_argument("wrapping E820 physical range");
        candidate.records[i] = {address, size, little32(offset+16), little32(offset+20)};
    }
    candidate.count = count;
    output = candidate;
    return Status::success();
}
} // namespace ok::boot
