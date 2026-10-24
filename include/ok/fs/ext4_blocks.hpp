#pragma once

#include "ok/fs/vfs.hpp"

namespace ok::fs
{
// Pure candidate qualification for the existing plain, nonjournalled subset.
// The caller has admitted superblock features and supplies the same snapshot's
// geometry and metadata-exclusion predicate. No I/O, time sample or write occurs.
template <typename DataRange>
    requires requires(DataRange range, u32 start, u32 count) { { range(start, count) } -> std::same_as<bool>; }
[[nodiscard]] Result<u64> ext4_allocated_blocks512(std::span<const std::byte> inode,
                                                  u32 block_size, u32 block_count,
                                                  DataRange data_range)
{
    if ((inode.size() != 128 && inode.size() != 256) ||
        (block_size != 1024 && block_size != 2048 && block_size != 4096) || !block_count)
        return Status::invalid_argument("ext4 allocation qualification geometry");
    const auto r16 = [&](usize at) -> u16 {
        return static_cast<u16>(std::to_integer<u8>(inode[at])) |
            (static_cast<u16>(std::to_integer<u8>(inode[at + 1])) << 8);
    };
    const auto r32 = [&](usize at) -> u32 { return r16(at) | (static_cast<u32>(r16(at + 2)) << 16); };
    const auto corrupt = [] { return Status::io_error("invalid ext4 allocated block metadata"); };
    const auto unsupported = [] { return Status::unsupported("ext4 allocated block layout unavailable"); };
    constexpr u32 extents = 0x80000;
    const u32 type = r16(0) & mode_type_mask, size = r32(4), sectors = r32(28), flags = r32(32);
    if (type != mode_regular && type != mode_directory && type != mode_symlink)
        return unsupported();
    // EA, large size, high sector/address representation and huge-file units
    // need separate accounting. Neither a low-word count nor zero is a fallback.
    if (r32(104) || r32(108) || r16(116) || r16(118)) return unsupported();
    const auto range_ok = [&](u32 start, u32 count) {
        return count && start < block_count && count <= block_count - start && data_range(start, count);
    };
    const auto inline_extents = [&]() -> Status {
        const u16 entries = r16(42), maximum = r16(44), depth = r16(46);
        // Reduced root capacity and nonzero depth are outside this canonical
        // inline subset, not sufficient evidence of filesystem corruption.
        if (r16(40) != 0xf30a || !maximum || maximum > 4 || entries > maximum ||
            depth > 32 || (depth && !entries)) return corrupt();
        if (maximum != 4 || depth) return unsupported();
        return Status::success();
    };
    if (type == mode_symlink)
    {
        if (flags != 0 && flags != extents) return unsupported();
        if (!size || size >= block_size) return corrupt();
        if (flags == extents)
            if (auto status = inline_extents(); !status) return status;
        if (!sectors)
        {
            if (flags || size >= 60) return corrupt();
            return u64{0};
        }
        if (sectors != block_size / 512) return corrupt();
        u32 physical;
        if (flags == extents)
        {
            if (r16(42) != 1 || r32(52) || r16(56) != 1 || r16(58)) return corrupt();
            physical = r32(60);
        }
        else
        {
            physical = r32(40);
            for (usize at = 44; at < 100; at += 4) if (r32(at)) return corrupt();
        }
        if (!range_ok(physical, 1)) return corrupt();
        return static_cast<u64>(sectors);
    }
    if (flags != extents) return unsupported();
    if (auto status = inline_extents(); !status) return status;
    const u64 logical_limit = (static_cast<u64>(size) + block_size - 1) / block_size;
    u64 previous_end = 0, allocated = 0;
    for (u16 index = 0; index < r16(42); ++index)
    {
        const usize at = 52 + index * 12;
        const u32 logical = r32(at), physical = r32(at + 8), length = r16(at + 4);
        if (length > 32768 || r16(at + 6)) return unsupported();
        if (!length || logical < previous_end || !range_ok(physical, length)) return corrupt();
        previous_end = static_cast<u64>(logical) + length;
        if (previous_end > (u64{1} << 32)) return corrupt();
        if (previous_end > logical_limit) return unsupported();
        for (u16 earlier = 0; earlier < index; ++earlier)
        {
            const usize other = 52 + earlier * 12;
            const u32 begin = r32(other + 8), count = r16(other + 4);
            if (physical < static_cast<u64>(begin) + count && begin < static_cast<u64>(physical) + length)
                return corrupt();
        }
        allocated += length;
    }
    if (allocated * (block_size / 512) != sectors) return corrupt();
    return static_cast<u64>(sectors);
}
} // namespace ok::fs
