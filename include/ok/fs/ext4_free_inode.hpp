#pragma once

#include "ok/fs/ext4_readonly.hpp"

namespace ok::fs
{
// Exact private free-slot format. A retained generation is advanced by reclaim
// and consumed by create; the allocated record never retains this marker.
class Ext4FreeInode final
{
    static void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (i * 8)); }
  public:
    static constexpr u32 marker = 0x3146524fU;
    [[nodiscard]] static Status encode(std::span<u8> inode, u32 number, u32 seed, u32 generation)
    {
        if (inode.size() != 256 || !number || !generation)
            return Status::invalid_argument("free inode marker identity or size");
        for (u8 &byte : inode) byte = 0;
        store32(inode, 100, generation); store16(inode, 128, 32);
        store32(inode, 160, marker);
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
        return {};
    }
    [[nodiscard]] static Result<u32> inspect(std::span<const u8> inode, u32 number, u32 seed)
    {
        if (inode.size() != 256 || !number)
            return Status::invalid_argument("free inode slot identity or size");
        if (jbd2::detail::all_zero(inode)) return u32{0};
        const u32 generation = ext4_checked::le32(inode, 100);
        if (!generation || ext4_checked::le32(inode, 160) != marker)
            return Status::unsupported("free inode marker or generation differs");
        if (auto status = ext4_checked::inode_checksum(seed, number, inode); !status) return status;
        std::array<u8, 256> expected{};
        if (auto status = encode(expected, number, seed, generation); !status) return status;
        for (usize i = 0; i < expected.size(); ++i)
            if (inode[i] != expected[i])
                return Status::unsupported("free inode marker has noncanonical bytes");
        // Keep one generation available for retirement of the next identity.
        if (generation == ~u32{0}) return Status::overflow("free inode generation exhausted");
        return generation;
    }
};
} // namespace ok::fs
