#pragma once

// ext4 extent module: extent-backed data planning: first and next data blocks, zero-length
// truncate and allocated-empty data, with their witnesses, previews and journal
// records.

#include "ok/fs/ext4/journal.hpp"
#include "ok/fs/ext4/layout.hpp"
#include "ok/block/sessions.hpp"
#include <type_traits>

namespace ok::fs
{
using Ext4FirstDataBlock = std::array<u8, 1024>;

struct Ext4FirstDataUpdate
{
    u32 inode{}, size{}, seconds{}, nanoseconds{};
};

// All images belong to one immutable, exclusive checked view. The caller must
// separately prove that no other allocated inode references the selected block.
struct Ext4FirstDataSource
{
    u32 group{}, physical{}, inode_table_block{};
    const Ext4FirstDataBlock &superblock;
    const Ext4FirstDataBlock &descriptors;
    const Ext4FirstDataBlock &block_bitmap;
    const Ext4FirstDataBlock &inode_table;
};

struct Ext4FirstDataPlan
{
    u32 inode{}, physical{};
    std::array<u32, 4> targets{};
    std::array<Ext4FirstDataBlock, 4> before{}, after{};
    usize count{};
};

// Pure image planning for the first initialized block of a canonical newly
// created file. Checked free-slot reuse preserves its new inode generation.
// No device I/O, journal authority, or no-reference proof is provided here.
class Ext4FirstDataPlanner final
{
    friend class Ext4AllocatedEmptyDataPlanner;
    friend class Ext4AllocatedEmptyDataWitnessCodec;
    friend class Ext4ZeroTruncatePlanner;
    friend class Ext4ZeroTruncateOwnershipValidator;
    friend class Ext4ZeroTruncateWitnessCodec;
    using Block = Ext4FirstDataBlock;
    struct Geometry
    {
        u32 groups{}, blocks_per_group{}, inodes{}, inodes_per_group{}, first_inode{};
        u32 table_blocks{}, seed{};
    };

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 number, std::span<const u8> bytes)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = bytes[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(number)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding, const Block &superblock,
                                        const Block &descriptors, Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.filesystem_blocks < 3 || binding.journal_inode != 8 ||
            binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block ||
            binding.metadata_count > binding.metadata.size() || !binding.metadata_count ||
            binding.metadata_count % 4 || !binding.structural_metadata().valid())
            return Status::unsupported("first data binding layout");
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto one = binding.metadata[index];
            if (one.first < binding.layout.journal_first_block + binding.layout.journal_blocks &&
                binding.layout.journal_first_block < one.first + one.count)
                return Status::io_error("first data metadata aliases journal");
            for (usize previous = 0; previous < index; ++previous)
            {
                const auto other = binding.metadata[previous];
                if (one.first < other.first + other.count && other.first < one.first + one.count)
                    return Status::io_error("first data metadata ranges overlap");
            }
        }
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("first data superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) || le32(s, 20) != 1 ||
            le32(s, 4) != binding.layout.filesystem_blocks || le32(s, 76) != 1 || le32(s, 72) ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le16(s, 88) != 256 || le16(s, 90) || le16(s, 58) != 1 ||
            le16(s, 206) || le32(s, 224) != 8 || le32(s, 228) || le32(s, 232) ||
            s[373] != 1 || (le16(s, 254) != 0 && le16(s, 254) != 32) ||
            le32(s, 336) || le32(s, 340) || le32(s, 344) ||
            !jbd2::detail::all_zero(s.subspan(208, 16)))
            return Status::unsupported("first data superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("first data filesystem UUID");
        geometry.blocks_per_group = le32(s, 32);
        geometry.inodes = le32(s, 0);
        geometry.inodes_per_group = le32(s, 40);
        geometry.first_inode = le32(s, 84);
        if (!geometry.blocks_per_group || geometry.blocks_per_group > 8192 ||
            geometry.blocks_per_group % 8 || !geometry.inodes_per_group ||
            geometry.inodes_per_group > 8192 || geometry.inodes_per_group % 8 ||
            geometry.inodes_per_group % 4 || le32(s, 28) ||
            le32(s, 36) != geometry.blocks_per_group ||
            geometry.first_inode < 11 || geometry.first_inode > geometry.inodes ||
            le32(s, 12) > binding.layout.filesystem_blocks - 1 || le32(s, 16) > geometry.inodes)
            return Status::io_error("first data group geometry");
        geometry.groups = u32((binding.layout.filesystem_blocks - 2) / geometry.blocks_per_group + 1);
        geometry.table_blocks = geometry.inodes_per_group / 4;
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (geometry.groups > Ext4RecoveryInfo::group_capacity ||
            geometry.groups * 4 != binding.metadata_count ||
            u64{geometry.groups} * geometry.inodes_per_group != geometry.inodes)
            return Status::unsupported("first data group capacity");
        u32 free_total = 0;
        for (u32 number = 0; number < geometry.groups; ++number)
        {
            const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
            if (descriptor_checksum(geometry.seed, number, d) != le16(d, 30))
                return Status::io_error("first data descriptor checksum");
            const u32 begin = 1 + number * geometry.blocks_per_group;
            const u32 count = u32(binding.layout.filesystem_blocks - begin < geometry.blocks_per_group
                ? binding.layout.filesystem_blocks - begin : geometry.blocks_per_group);
            const std::array<jbd2::BlockRange, 4> expected{{{begin, 2}, {le32(d, 0), 1},
                                                              {le32(d, 4), 1}, {le32(d, 8), geometry.table_blocks}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (expected[index].first < begin ||
                    expected[index].first >= u64{begin} + count ||
                    expected[index].count > u64{begin} + count - expected[index].first ||
                    binding.metadata[number * 4 + index].first != expected[index].first ||
                    binding.metadata[number * 4 + index].count != expected[index].count)
                    return Status::unsupported("first data group placement changed");
            const u16 flags = le16(d, 18), free_blocks = le16(d, 12);
            if (count < 2 || (flags & ~u16{7}) || free_blocks > count ||
                le16(d, 14) > geometry.inodes_per_group ||
                le16(d, 16) > geometry.inodes_per_group - le16(d, 14) ||
                le16(d, 28) > geometry.inodes_per_group ||
                ((flags & 1) && (le16(d, 14) != geometry.inodes_per_group ||
                                 le16(d, 16) || le16(d, 28) != geometry.inodes_per_group || le16(d, 26))) ||
                ((flags & 2) && (count < geometry.table_blocks + 4 ||
                                 free_blocks != count - geometry.table_blocks - 4 || le16(d, 24))))
                return Status::io_error("first data group counters or flags");
            free_total += free_blocks;
        }
        if (free_total != le32(s, 12))
            return Status::io_error("first data free block counters disagree");
        return {};
    }

  public:
    [[nodiscard]] static Result<u32> select_block(const Ext4RecoveryInfo &binding,
                                                   const Block &superblock, const Block &descriptors,
                                                   const Block &block_bitmap, u32 group_number)
    {
        using namespace ext4_checked;
        Geometry geometry{};
        if (auto status = inspect(binding, superblock, descriptors, geometry); !status) return status;
        if (group_number >= geometry.groups)
            return Status::invalid_argument("first data group number");
        u32 first_supported = geometry.groups;
        bool unsupported_space = false;
        for (u32 number = 0; number < geometry.groups; ++number)
        {
            const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
            if (!le16(d, 12)) continue;
            if (le16(d, 18) & 2) { unsupported_space = true; continue; }
            first_supported = number;
            break;
        }
        if (first_supported == geometry.groups)
            return unsupported_space ? Status::unsupported("first data only uninitialized groups have space") :
                                       Status::no_space("first data filesystem has no free block");
        if (group_number != first_supported)
            return Status::invalid_argument("first data group is not the first supported free group");
        const auto d = std::span<const u8>{descriptors}.subspan(group_number * 32, 32);
        const u32 begin = 1 + group_number * geometry.blocks_per_group;
        const u32 count = u32(binding.layout.filesystem_blocks - begin < geometry.blocks_per_group
            ? binding.layout.filesystem_blocks - begin : geometry.blocks_per_group);
        const auto bitmap = std::span<const u8>{block_bitmap}.first(geometry.blocks_per_group / 8);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != le16(d, 24))
            return Status::io_error("first data block bitmap checksum");
        u32 free_count = 0, candidate = 0;
        for (u32 local = 0; local < geometry.blocks_per_group; ++local)
        {
            const bool allocated = (bitmap[local / 8] & (1U << (local % 8))) != 0;
            if (local >= count)
            {
                if (!allocated) return Status::io_error("first data final group padding bit is clear");
                continue;
            }
            const u32 physical = begin + local;
            if (!ordinary_block(binding, physical))
            {
                if (!allocated) return Status::io_error("first data reserved block bit is clear");
                continue;
            }
            if (!allocated)
            {
                ++free_count;
                if (!candidate) candidate = physical;
            }
        }
        if (free_count != le16(d, 12))
            return Status::io_error("first data bitmap count differs from descriptor");
        if (!candidate) return Status::io_error("first data free count has no ordinary block");
        return candidate;
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4FirstDataUpdate &update,
                                     const Ext4FirstDataSource &source, Ext4FirstDataPlan &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!update.size || update.size > 1024 || update.seconds > 0x7fffffffU ||
            update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("first data size or timestamp");
        Geometry geometry{};
        if (auto status = inspect(binding, source.superblock, source.descriptors, geometry); !status) return status;
        if (update.inode < geometry.first_inode || update.inode > geometry.inodes)
            return Status::invalid_argument("first data ordinary inode identity");
        const auto selected = select_block(binding, source.superblock, source.descriptors,
                                           source.block_bitmap, source.group);
        if (!selected) return selected.status();
        if (source.physical != selected.value())
            return Status::invalid_argument("first data block is not selected");
        const u32 inode_group = (update.inode - 1) / geometry.inodes_per_group;
        const u32 inode_local = (update.inode - 1) % geometry.inodes_per_group;
        const auto inode_descriptor = std::span<const u8>{source.descriptors}.subspan(inode_group * 32, 32);
        if (le16(inode_descriptor, 18) & 1)
            return Status::unsupported("first data inode bitmap is uninitialized");
        const u32 inode_table_block = le32(inode_descriptor, 8) + inode_local / 4;
        const u32 bitmap_block = le32(std::span<const u8>{source.descriptors}.subspan(source.group * 32, 32), 0);
        if (source.inode_table_block != inode_table_block || bitmap_block == 2 || bitmap_block == 1 ||
            inode_table_block == bitmap_block || inode_table_block == 2 || inode_table_block == 1)
            return Status::unsupported("first data participant placement or alias");
        const usize inode_offset = (inode_local % 4) * 256;
        const auto inode = std::span<const u8>{source.inode_table}.subspan(inode_offset, 256);
        if (auto status = inode_checksum(geometry.seed, update.inode, inode); !status) return status;
        const u16 mode = le16(inode, 0);
        const u32 created_seconds = le32(inode, 8), created_extra = le32(inode, 140);
        if ((mode & 0170000) != 0100000 || (mode & 07000) || le16(inode, 26) != 1 ||
            created_seconds > 0x7fffffffU || (created_extra & 3) || created_extra >> 2 >= 1000000000U)
            return Status::unsupported("first data requires a canonical fresh regular inode");
        std::array<u8, 256> canonical{};
        store16(canonical, 0, mode);
        store16(canonical, 2, le16(inode, 2));
        store16(canonical, 120, le16(inode, 120));
        store16(canonical, 24, le16(inode, 24));
        store16(canonical, 122, le16(inode, 122));
        store16(canonical, 26, 1);
        for (const usize at : {usize{8}, usize{12}, usize{16}, usize{144}})
            store32(canonical, at, created_seconds);
        for (const usize at : {usize{140}, usize{132}, usize{136}, usize{148}})
            store32(canonical, at, created_extra);
        store32(canonical, 32, 0x80000);
        // Generation is part of the checked allocated source identity and
        // inode checksum. Allocation does not reset it or infer provenance
        // from a public generation number.
        store32(canonical, 100, le32(inode, 100));
        store16(canonical, 40, 0xf30a); store16(canonical, 44, 4);
        store16(canonical, 128, 32);
        stamp_inode(canonical, update.inode, geometry.seed);
        for (usize index = 0; index < inode.size(); ++index)
            if (inode[index] != canonical[index])
                return Status::unsupported("first data source inode differs from canonical create");

        Ext4FirstDataPlan prepared{};
        prepared.inode = update.inode;
        prepared.physical = source.physical;
        prepared.targets = {bitmap_block, 2, 1, inode_table_block};
        prepared.before = {source.block_bitmap, source.descriptors, source.superblock, source.inode_table};
        prepared.after = prepared.before;
        prepared.count = 4;
        const u32 local = source.physical - (1 + source.group * geometry.blocks_per_group);
        prepared.after[0][local / 8] |= u8(1U << (local % 8));
        auto descriptor_after = std::span<u8>{prepared.after[1]}.subspan(source.group * 32, 32);
        store16(descriptor_after, 12, u16(le16(descriptor_after, 12) - 1));
        store16(descriptor_after, 24, u16(jbd2::detail::crc32c(
            geometry.seed, std::span<const u8>{prepared.after[0]}.first(geometry.blocks_per_group / 8))));
        store16(descriptor_after, 30, 0);
        store16(descriptor_after, 30, descriptor_checksum(geometry.seed, source.group, descriptor_after));
        store32(prepared.after[2], 12, le32(source.superblock, 12) - 1);
        store32(prepared.after[2], 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{prepared.after[2]}.first(1020)));
        auto inode_after = std::span<u8>{prepared.after[3]}.subspan(inode_offset, 256);
        store32(inode_after, 4, update.size);
        store32(inode_after, 12, update.seconds); store32(inode_after, 132, update.nanoseconds << 2);
        store32(inode_after, 16, update.seconds); store32(inode_after, 136, update.nanoseconds << 2);
        store32(inode_after, 28, 2); store32(inode_after, 36, 1);
        store16(inode_after, 42, 1); store16(inode_after, 56, 1);
        store32(inode_after, 60, source.physical);
        stamp_inode(inode_after, update.inode, geometry.seed);
        output = prepared;
        return {};
    }
};

using Ext4NextDataBlock = Ext4FirstDataBlock;

struct Ext4NextDataUpdate
{
    u32 inode{}, old_size{}, size{}, seconds{}, nanoseconds{};
};

struct Ext4NextDataSource
{
    u32 group{}, physical{}, inode_table_block{};
    const Ext4NextDataBlock &superblock;
    const Ext4NextDataBlock &descriptors;
    const Ext4NextDataBlock &block_bitmap;
    const Ext4NextDataBlock &inode_table;
};

struct Ext4NextDataPrefix
{
    u32 old_blocks{}, physical{};
    std::array<u32, 15> old_physical{};
};

struct Ext4NextDataPlan
{
    u32 inode{}, physical{}, old_blocks{};
    std::array<u32, 15> old_physical{};
    std::array<u32, 4> targets{};
    std::array<Ext4NextDataBlock, 4> before{}, after{};
    usize count{};
};

// Pure image planning. The owner must separately check allocation and unique
// ownership of every old block and the free candidate before any data I/O.
class Ext4NextDataPlanner final
{
    friend class Ext4ZeroTruncatePlanner;
    using Block = Ext4NextDataBlock;
    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 number, std::span<const u8> bytes)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = bytes[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(number)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static bool valid_time(std::span<const u8> inode, usize base, usize extra)
    {
        using namespace ext4_checked;
        const u32 extension = le32(inode, extra);
        return le32(inode, base) <= 0x7fffffffU && !(extension & 3) &&
               (extension >> 2) < 1000000000U;
    }
    [[nodiscard]] static Status parse_inode(const Ext4RecoveryInfo &binding, u32 number,
                                            std::span<const u8> inode, u32 expected_blocks,
                                            std::array<u32, 16> &mapped)
    {
        using namespace ext4_checked;
        if (expected_blocks < 1 || expected_blocks > 16 ||
            (le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) != 1 || le32(inode, 4) == 0 || le32(inode, 4) > 16384 ||
            le32(inode, 20) || le32(inode, 28) != expected_blocks * 2 ||
            le32(inode, 32) != 0x80000 ||
            le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le16(inode, 128) != 32 || le32(inode, 156) ||
            le16(inode, 40) != 0xf30a || !le16(inode, 42) || le16(inode, 42) > 4 ||
            le16(inode, 44) != 4 || le16(inode, 46) || le32(inode, 48) ||
            !valid_time(inode, 8, 140) || !valid_time(inode, 12, 132) ||
            !valid_time(inode, 16, 136) || !valid_time(inode, 144, 148))
            return Status::unsupported("next data inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("next data inode extra fields");
        const u32 count = le16(inode, 42);
        for (usize at = 52 + count * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("next data unused extent fields");
        if (auto status = inode_checksum(jbd2::detail::crc32c(~u32{0}, binding.uuid), number, inode);
            !status) return status;
        u32 next = 0;
        for (u32 index = 0; index < count; ++index)
        {
            const usize at = 52 + index * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > 32768 || logical != next || length > expected_blocks - next ||
                le16(inode, at + 6) || !physical || u64{physical} + length > binding.layout.filesystem_blocks)
                return Status::unsupported("next data requires exact initialized inline extents");
            for (u32 part = 0; part < length; ++part)
            {
                const u32 block = physical + part;
                if (!ordinary_block(binding, block))
                    return Status::unsupported("next data prior extent aliases reserved block");
                for (u32 previous = 0; previous < next + part; ++previous)
                    if (mapped[previous] == block)
                        return Status::io_error("next data prior physical extent overlaps");
                mapped[next + part] = block;
            }
            next += length;
        }
        if (next != expected_blocks)
            return Status::unsupported("next data extent coverage");
        return {};
    }

  public:
    [[nodiscard]] static Result<u32> select_block(const Ext4RecoveryInfo &binding,
        const Block &superblock, const Block &descriptors, const Block &block_bitmap, u32 group)
    {
        return Ext4FirstDataPlanner::select_block(binding, superblock, descriptors, block_bitmap, group);
    }

    [[nodiscard]] static Status inspect_final_inode(const Ext4RecoveryInfo &binding, u32 number,
                                                     const Block &inode_table, usize inode_offset,
                                                     u32 candidate, Ext4NextDataPrefix &output)
    {
        using namespace ext4_checked;
        output = {};
        if (inode_offset > 768 || inode_offset % 256 || !ordinary_block(binding, candidate))
            return Status::invalid_argument("next data final inode or candidate");
        const auto inode = std::span<const u8>{inode_table}.subspan(inode_offset, 256);
        const u32 size = le32(inode, 4);
        if (le32(inode, 12) != le32(inode, 16) || le32(inode, 132) != le32(inode, 136) ||
            !(le32(inode, 36) || le32(inode, 152)))
            return Status::unsupported("next data final timestamp or incremented version");
        if (size <= 1024 || size > 16384)
            return Status::unsupported("next data final size");
        const u32 final_blocks = (size + 1023) / 1024;
        if (final_blocks < 2 || final_blocks > 16) return Status::unsupported("next data final block count");
        std::array<u32, 16> mapped{};
        if (auto status = parse_inode(binding, number, inode, final_blocks, mapped); !status) return status;
        if ((le32(inode, 36) == 0 && le32(inode, 152) == 0) ||
            le32(inode, 12) != le32(inode, 16) || le32(inode, 132) != le32(inode, 136))
            return Status::unsupported("next data final version or write timestamp");
        if (mapped[final_blocks - 1] != candidate)
            return Status::unsupported("next data final candidate mapping");
        // Removing the last logical block must be exactly one coalescence or
        // exactly one appended record; both preserve the complete old prefix.
        const u32 records = le16(inode, 42);
        const usize last_at = 52 + (records - 1) * 12;
        if (le16(inode, last_at + 4) == 1 &&
            (le32(inode, last_at) != final_blocks - 1 ||
             mapped[final_blocks - 2] + u64{1} == candidate))
            return Status::unsupported("next data final appended extent is not canonical");
        if (le16(inode, last_at + 4) > 1 &&
            mapped[final_blocks - 2] + u64{1} != candidate)
            return Status::unsupported("next data final coalesced extent");
        output.old_blocks = final_blocks - 1;
        output.physical = candidate;
        for (u32 index = 0; index < output.old_blocks; ++index)
            output.old_physical[index] = mapped[index];
        return {};
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4NextDataUpdate &update,
                                     const Ext4NextDataSource &source, Ext4NextDataPlan &output)
    {
        using namespace ext4_checked;
        output = {};
        if (update.old_size < 1024 || update.old_size > 15360 || update.old_size % 1024 ||
            update.size <= update.old_size || u64{update.size} > u64{update.old_size} + 1024 ||
            update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("next data size or timestamp");
        const auto selected = Ext4FirstDataPlanner::select_block(binding, source.superblock,
            source.descriptors, source.block_bitmap, source.group);
        if (!selected) return selected.status();
        if (source.physical != selected.value())
            return Status::invalid_argument("next data block is not selected");
        const u32 inodes = le32(source.superblock, 0), inodes_per_group = le32(source.superblock, 40);
        const u32 first_inode = le32(source.superblock, 84);
        if (update.inode < first_inode || update.inode > inodes || !inodes_per_group)
            return Status::invalid_argument("next data ordinary inode identity");
        const u32 inode_group = (update.inode - 1) / inodes_per_group;
        const u32 inode_local = (update.inode - 1) % inodes_per_group;
        const auto inode_descriptor = std::span<const u8>{source.descriptors}.subspan(inode_group * 32, 32);
        if (le16(inode_descriptor, 18) & 1)
            return Status::unsupported("next data inode bitmap is uninitialized");
        const u32 table = le32(inode_descriptor, 8) + inode_local / 4;
        const u32 bitmap = le32(std::span<const u8>{source.descriptors}.subspan(source.group * 32, 32), 0);
        if (source.inode_table_block != table || table == bitmap || table == 1 || table == 2 ||
            bitmap == 1 || bitmap == 2 || table == source.physical)
            return Status::unsupported("next data participant placement or alias");
        const usize inode_offset = (inode_local % 4) * 256;
        const auto inode = std::span<const u8>{source.inode_table}.subspan(inode_offset, 256);
        if (le32(inode, 4) != update.old_size)
            return Status::unsupported("next data old size differs from inode");
        const u64 version = u64{le32(inode, 36)} | (u64{le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("next data inode version exhausted");
        const u32 old_blocks = update.old_size / 1024;
        std::array<u32, 16> old_mapping{};
        if (auto status = parse_inode(binding, update.inode, inode, old_blocks, old_mapping); !status)
            return status;
        Ext4NextDataPlan prepared{};
        prepared.inode = update.inode;
        prepared.physical = source.physical;
        prepared.old_blocks = old_blocks;
        for (u32 index = 0; index < old_blocks; ++index)
            prepared.old_physical[index] = old_mapping[index];
        prepared.targets = {bitmap, 2, 1, table};
        prepared.before = {source.block_bitmap, source.descriptors, source.superblock, source.inode_table};
        prepared.after = prepared.before;
        prepared.count = 4;
        const u32 local = source.physical - (1 + source.group * le32(source.superblock, 32));
        prepared.after[0][local / 8] |= u8(1U << (local % 8));
        auto descriptor_after = std::span<u8>{prepared.after[1]}.subspan(source.group * 32, 32);
        store16(descriptor_after, 12, u16(le16(descriptor_after, 12) - 1));
        store16(descriptor_after, 24, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(~u32{0}, binding.uuid),
            std::span<const u8>{prepared.after[0]}.first(le32(source.superblock, 32) / 8))));
        store16(descriptor_after, 30, 0);
        store16(descriptor_after, 30, descriptor_checksum(
            jbd2::detail::crc32c(~u32{0}, binding.uuid), source.group, descriptor_after));
        store32(prepared.after[2], 12, le32(source.superblock, 12) - 1);
        store32(prepared.after[2], 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{prepared.after[2]}.first(1020)));
        auto inode_after = std::span<u8>{prepared.after[3]}.subspan(inode_offset, 256);
        store32(inode_after, 4, update.size);
        store32(inode_after, 12, update.seconds); store32(inode_after, 132, update.nanoseconds << 2);
        store32(inode_after, 16, update.seconds); store32(inode_after, 136, update.nanoseconds << 2);
        store32(inode_after, 28, (old_blocks + 1) * 2);
        store32(inode_after, 36, u32(version + 1));
        store32(inode_after, 152, u32((version + 1) >> 32));
        const u32 records = le16(inode_after, 42);
        const usize last_at = 52 + (records - 1) * 12;
        const u32 prior_start = le32(inode_after, last_at + 8);
        const u32 prior_length = le16(inode_after, last_at + 4);
        if (u64{prior_start} + prior_length == source.physical && prior_length < 32768)
            store16(inode_after, last_at + 4, u16(prior_length + 1));
        else
        {
            if (records == 4) return Status::unsupported("next data requires a fifth inline extent");
            store16(inode_after, 42, u16(records + 1));
            const usize at = 52 + records * 12;
            store32(inode_after, at, old_blocks);
            store16(inode_after, at + 4, 1);
            store32(inode_after, at + 8, source.physical);
        }
        stamp_inode(inode_after, update.inode, jbd2::detail::crc32c(~u32{0}, binding.uuid));
        Ext4NextDataPrefix final{};
        if (auto status = inspect_final_inode(binding, update.inode, prepared.after[3],
                                               inode_offset, source.physical, final); !status) return status;
        if (final.old_blocks != old_blocks || final.old_physical != prepared.old_physical)
            return Status::fault("next data final mapping differs from old prefix");
        output = prepared;
        return {};
    }
};

using Ext4ZeroTruncateBlock = Ext4FirstDataBlock;

struct Ext4ZeroTruncateUpdate
{
    u32 inode{}, generation{}, old_size{}, seconds{}, nanoseconds{};
};
struct Ext4ZeroTruncateSource
{
    u32 group{}, inode_table_block{};
    const Ext4ZeroTruncateBlock &superblock, &descriptors, &block_bitmap, &inode_bitmap, &inode_table;
};
struct Ext4ZeroTruncatePlan
{
    u32 inode{}, generation{}, old_size{}, group{}, released_count{};
    std::array<u32, 16> released_mapping{};
    std::array<u32, 4> targets{};
    std::array<Ext4ZeroTruncateBlock, 4> before{}, after{};
    usize count{};
    bool noop{};
};

// Pure four-home transformation. Neither a public plan nor its mapping grants
// mutation authority; the mounted owner must prove lifetime and full ownership.
class Ext4ZeroTruncatePlanner final
{
    using Parent = Ext4FirstDataPlanner;
    using Block = Ext4ZeroTruncateBlock;
    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    [[nodiscard]] static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
  public:
    // This profile is shared by preflight, witness regeneration and empty-file
    // reallocation. It never infers fresh-create provenance from equal times.
    [[nodiscard]] static Status inspect_inode(const Ext4RecoveryInfo &binding, u32 number,
        std::span<const u8, 256> inode, std::array<u32, 16> &mapping, u32 &blocks)
    {
        using namespace ext4_checked;
        mapping = {}; blocks = 0;
        const u32 size = le32(inode, 4);
        if (size > 16384) return Status::unsupported("zero truncate data-block capacity");
        blocks = u32((u64{size} + 1023) / 1024);
        if (blocks)
            return Ext4NextDataPlanner::parse_inode(binding, number, inode, blocks, mapping);
        if (auto status = inode_checksum(jbd2::detail::crc32c(~u32{0}, binding.uuid), number, inode);
            !status) return status;
        if ((le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) != 1 || le32(inode, 20) || le32(inode, 28) ||
            le32(inode, 32) != 0x80000 || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le16(inode, 128) != 32 || le32(inode, 156) ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) || le16(inode, 44) != 4 ||
            le16(inode, 46) || le32(inode, 48) ||
            !Ext4NextDataPlanner::valid_time(inode, 8, 140) ||
            !Ext4NextDataPlanner::valid_time(inode, 12, 132) ||
            !Ext4NextDataPlanner::valid_time(inode, 16, 136) ||
            !Ext4NextDataPlanner::valid_time(inode, 144, 148))
            return Status::unsupported("zero truncate canonical allocated-empty inode");
        for (usize at = 52; at < 100; ++at)
            if (inode[at]) return Status::unsupported("zero truncate empty extent fields");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("zero truncate inode extra fields");
        return {};
    }

    [[nodiscard]] static Status plan_off_stack(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateUpdate &update, const Ext4ZeroTruncateSource &source,
        Ext4ZeroTruncatePlan &scratch, Ext4ZeroTruncatePlan &output)
    {
        using namespace ext4_checked;
        if (overlaps(&scratch, sizeof(scratch), &output, sizeof(output)))
            return Status::invalid_argument("zero truncate scratch/output overlap");
        if (overlaps(&scratch, sizeof(scratch), &update, sizeof(update)) ||
            overlaps(&output, sizeof(output), &update, sizeof(update)) ||
            overlaps(&scratch, sizeof(scratch), &binding, sizeof(binding)) ||
            overlaps(&output, sizeof(output), &binding, sizeof(binding)) ||
            overlaps(&scratch, sizeof(scratch), &source, sizeof(source)) ||
            overlaps(&output, sizeof(output), &source, sizeof(source)))
            return Status::invalid_argument("zero truncate request/workspace overlap");
        for (const auto *image : {&source.superblock, &source.descriptors, &source.block_bitmap,
                                  &source.inode_bitmap, &source.inode_table})
            if (overlaps(&scratch, sizeof(scratch), image, sizeof(*image)) ||
                overlaps(&output, sizeof(output), image, sizeof(*image)))
                return Status::invalid_argument("zero truncate source/workspace overlap");
        scratch = {}; output = {};
        Parent::Geometry geometry{};
        if (auto status = Parent::inspect(binding, source.superblock, source.descriptors, geometry);
            !status) return status;
        if (update.inode < geometry.first_inode || update.inode > geometry.inodes || source.group >= geometry.groups)
            return Status::invalid_argument("zero truncate ordinary inode or data group");
        const u32 inode_group = (update.inode - 1) / geometry.inodes_per_group;
        const u32 inode_local = (update.inode - 1) % geometry.inodes_per_group;
        const auto id = std::span<const u8>{source.descriptors}.subspan(inode_group * 32, 32);
        const auto dd = std::span<const u8>{source.descriptors}.subspan(source.group * 32, 32);
        if ((le16(id, 18) & 1) || (le16(dd, 18) & 2))
            return Status::unsupported("zero truncate initialized inode and data groups required");
        const usize inode_bitmap_bytes = geometry.inodes_per_group / 8;
        if (u16(jbd2::detail::crc32c(geometry.seed,
                std::span<const u8>{source.inode_bitmap}.first(inode_bitmap_bytes))) != le16(id, 26))
            return Status::io_error("zero truncate inode bitmap checksum");
        u32 free_inodes = 0;
        for (u32 local = 0; local < geometry.inodes_per_group; ++local)
            if (!(source.inode_bitmap[local / 8] & (1U << (local % 8)))) ++free_inodes;
        if (free_inodes != le16(id, 14) ||
            !(source.inode_bitmap[inode_local / 8] & (1U << (inode_local % 8))))
            return Status::io_error("zero truncate inode allocation or free count");
        const u32 table = le32(id, 8) + inode_local / 4;
        const u32 bitmap = le32(dd, 0);
        if (source.inode_table_block != table || table == bitmap || table == 1 || table == 2 ||
            bitmap == 1 || bitmap == 2)
            return Status::unsupported("zero truncate home placement or alias");
        const auto inode = std::span<const u8, 256>{source.inode_table.data() + (inode_local % 4) * 256, 256};
        if (le32(inode, 100) != update.generation || le32(inode, 4) != update.old_size)
            return Status::io_error("zero truncate stale generation or old size");
        u32 blocks{};
        if (auto status = inspect_inode(binding, update.inode, inode, scratch.released_mapping, blocks);
            !status) return status;
        const u32 begin = 1 + source.group * geometry.blocks_per_group;
        const u32 count = u32(binding.layout.filesystem_blocks - begin < geometry.blocks_per_group
            ? binding.layout.filesystem_blocks - begin : geometry.blocks_per_group);
        const auto bits = std::span<const u8>{source.block_bitmap}.first(geometry.blocks_per_group / 8);
        if (u16(jbd2::detail::crc32c(geometry.seed, bits)) != le16(dd, 24))
            return Status::io_error("zero truncate block bitmap checksum");
        u32 free_blocks = 0;
        for (u32 local = 0; local < geometry.blocks_per_group; ++local)
        {
            const bool allocated = bits[local / 8] & (1U << (local % 8));
            if (local >= count || !Parent::ordinary_block(binding, begin + local))
            {
                if (!allocated) return Status::io_error("zero truncate reserved or padding bit is clear");
            }
            else if (!allocated) ++free_blocks;
        }
        if (free_blocks != le16(dd, 12))
            return Status::io_error("zero truncate bitmap and free count disagree");
        for (u32 index = 0; index < blocks; ++index)
        {
            const u32 physical = scratch.released_mapping[index];
            if ((physical - 1) / geometry.blocks_per_group != source.group)
                return Status::unsupported("zero truncate release crosses allocation groups");
            const u32 local = physical - begin;
            if (!(bits[local / 8] & (1U << (local % 8))))
                return Status::io_error("zero truncate released block is already free");
        }
        scratch.inode = update.inode; scratch.generation = update.generation;
        scratch.old_size = update.old_size; scratch.group = source.group;
        scratch.released_count = blocks;
        if (!blocks)
        {
            scratch.noop = true; output = scratch;
            return {};
        }
        if (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("zero truncate maintained timestamp");
        const u64 version = u64{le32(inode, 36)} | (u64{le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("zero truncate inode version exhausted");
        if (blocks > count - free_blocks || le32(source.superblock, 12) >
                binding.layout.filesystem_blocks - 1 - blocks)
            return Status::io_error("zero truncate free counter overflow");
        scratch.targets = {table, bitmap, 2, 1};
        const std::array<const Block *, 4> images{&source.inode_table, &source.block_bitmap,
                                                 &source.descriptors, &source.superblock};
        for (usize home = 0; home < images.size(); ++home)
            for (usize byte = 0; byte < 1024; ++byte) scratch.before[home][byte] = (*images[home])[byte];
        scratch.after = scratch.before; scratch.count = 4;
        auto after_inode = std::span<u8>{scratch.after[0]}.subspan((inode_local % 4) * 256, 256);
        store32(after_inode, 4, 0); store32(after_inode, 28, 0);
        store32(after_inode, 12, update.seconds); store32(after_inode, 132, update.nanoseconds << 2);
        store32(after_inode, 16, update.seconds); store32(after_inode, 136, update.nanoseconds << 2);
        store32(after_inode, 36, u32(version + 1)); store32(after_inode, 152, u32((version + 1) >> 32));
        for (usize at = 40; at < 100; ++at) after_inode[at] = 0;
        store16(after_inode, 40, 0xf30a); store16(after_inode, 44, 4);
        Parent::stamp_inode(after_inode, update.inode, geometry.seed);
        for (u32 index = 0; index < blocks; ++index)
        {
            const u32 local = scratch.released_mapping[index] - begin;
            scratch.after[1][local / 8] &= u8(~(1U << (local % 8)));
        }
        auto after_descriptor = std::span<u8>{scratch.after[2]}.subspan(source.group * 32, 32);
        store16(after_descriptor, 12, u16(free_blocks + blocks));
        store16(after_descriptor, 24, u16(jbd2::detail::crc32c(geometry.seed,
            std::span<const u8>{scratch.after[1]}.first(geometry.blocks_per_group / 8))));
        store16(after_descriptor, 30, 0);
        store16(after_descriptor, 30, Parent::descriptor_checksum(geometry.seed, source.group, after_descriptor));
        store32(scratch.after[3], 12, le32(source.superblock, 12) + blocks);
        store32(scratch.after[3], 1020, jbd2::detail::crc32c(~u32{0},
            std::span<const u8>{scratch.after[3]}.first(1020)));
        output = scratch;
        return {};
    }
};

struct Ext4ZeroTruncateOwnership
{
    u32 inode{}, generation{}, old_size{}, count{}, group{};
    std::array<u32, 16> mapping{};
};
struct Ext4ZeroTruncateOwnershipScratch
{
    Ext4GlobalOwnershipScratch global{};
    Ext4ZeroTruncateBlock superblock{}, descriptors{}, bitmap{};
    std::array<u8, 256> inode{};
};

// Read-only admission facts over an exclusive checked view. Full scans reject
// references from directories, symlinks, reserved records and journal metadata.
class Ext4ZeroTruncateOwnershipValidator final
{
#include "ok/fs/ext4/detail/completed_ownership_birth.inc"
  public:
    template <jbd2::BlockReader Reader>
    [[nodiscard]] static Status prove(const Ext4RecoveryInfo &binding, Ext4ReadOnlyView<Reader> &view,
        const Ext4ZeroTruncateUpdate &update, Ext4ZeroTruncateOwnershipScratch &scratch,
        Ext4ZeroTruncateOwnership &output)
    { return prove_impl<false>(binding, view, static_cast<const Reader *>(nullptr), update, scratch, output, nullptr, nullptr, nullptr); }
  private:
    template <jbd2::BlockReader Reader>
    [[nodiscard]] static Status prove_completed(const Ext4RecoveryInfo &binding, Ext4ReadOnlyView<Reader> &view,
        const Reader &reader, const Ext4ZeroTruncateUpdate &update, Ext4ZeroTruncateOwnershipScratch &scratch,
        Ext4ZeroTruncateOwnership &output, CompletedBirthSeal &seal, CompletedBirthProgress &progress, const void *child)
    {
        if (progress.stage_ != CompletedBirthProgress::Stage::unused || !child ||
            !completed_birth_progress_exact(seal, progress, child))
            return Status::already_exists("completed ownership birth is one-shot");
        progress.child_ = child; progress.seal_ = &seal; progress.stage_ = CompletedBirthProgress::Stage::proving;
        return prove_impl<true>(binding, view, &reader, update, scratch, output, &seal, &progress, child);
    }
    template <bool Capture, jbd2::BlockReader Reader>
    [[nodiscard]] static Status prove_impl(const Ext4RecoveryInfo &binding, Ext4ReadOnlyView<Reader> &view,
        const Reader *reader, const Ext4ZeroTruncateUpdate &update, Ext4ZeroTruncateOwnershipScratch &scratch,
        Ext4ZeroTruncateOwnership &output, CompletedBirthSeal *birth, CompletedBirthProgress *progress, const void *child)
    {
        using namespace ext4_checked;
        output = {};
        if (!view.valid()) return Status::not_initialized("zero truncate checked ownership view");
        if (binding.layout.block_size != 1024 || view.layout_.block_size != 1024 || binding.uuid != view.uuid_ ||
            binding.layout.filesystem_blocks != view.layout_.filesystem_blocks ||
            binding.layout.journal_first_block != view.layout_.journal_first_block ||
            binding.layout.journal_blocks != view.layout_.journal_blocks ||
            update.inode < view.first_inode_)
            return Status::invalid_argument("zero truncate ownership binding or inode identity");
        if (auto status = view.decode_inode(update.inode, scratch.inode, scratch.global.block); !status)
            return view.failure(status);
        if (le32(scratch.inode, 100) != update.generation || le32(scratch.inode, 4) != update.old_size)
            return Status::io_error("zero truncate ownership stale generation or size");
        Ext4ZeroTruncateOwnership prepared{};
        if (auto status = Ext4ZeroTruncatePlanner::inspect_inode(binding, update.inode, scratch.inode,
                prepared.mapping, prepared.count); !status) return status;
        if (auto status = view.prove_global_data_ownership(scratch.global); !status) return status;
        // The global range scan is semantically complete here. Seal it before
        // the descriptor/bitmap/target verification performs another callback.
        if constexpr (Capture)
            if (auto status = capture_completed_global(view, *reader, binding, output, *birth, *progress, child); !status) return status;
        if (auto status = view.block(1, scratch.superblock); !status) return status;
        if (auto status = view.block(2, scratch.descriptors); !status) return status;
        Ext4FirstDataPlanner::Geometry geometry{};
        if (auto status = Ext4FirstDataPlanner::inspect(binding, scratch.superblock, scratch.descriptors, geometry);
            !status) return view.failure(status);
        u32 total_free_inodes = 0;
        for (u32 group = 0; group < geometry.groups; ++group)
        {
            const auto d = std::span<const u8>{scratch.descriptors}.subspan(group * 32, 32);
            const auto &known = view.groups_[group];
            if (le32(d, 0) != known.block_bitmap || le32(d, 4) != known.inode_bitmap ||
                le32(d, 8) != known.table || le16(d, 18) != known.flags ||
                le16(d, 24) != known.block_checksum || le16(d, 26) != known.inode_checksum)
                return view.failure(Status::io_error("zero truncate ownership descriptor changed"));
            total_free_inodes += le16(d, 14);
            if (!(known.flags & 1))
            {
                if (auto status = view.block(known.inode_bitmap, scratch.bitmap); !status) return status;
                if (u16(jbd2::detail::crc32c(geometry.seed,
                    std::span<const u8>{scratch.bitmap}.first(geometry.inodes_per_group / 8))) != known.inode_checksum)
                    return view.failure(Status::io_error("zero truncate ownership inode bitmap checksum"));
                u32 free = 0;
                for (u32 bit = 0; bit < geometry.inodes_per_group; ++bit)
                    if (!(scratch.bitmap[bit / 8] & (1U << (bit % 8)))) ++free;
                if (free != le16(d, 14))
                    return view.failure(Status::io_error("zero truncate ownership inode free count"));
            }
            if (known.flags & 2) continue;
            if (auto status = view.block(known.block_bitmap, scratch.bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(geometry.seed,
                std::span<const u8>{scratch.bitmap}.first(geometry.blocks_per_group / 8))) != known.block_checksum)
                return view.failure(Status::io_error("zero truncate ownership block bitmap checksum"));
            const u32 begin = 1 + group * geometry.blocks_per_group;
            const u32 count = u32(binding.layout.filesystem_blocks - begin < geometry.blocks_per_group
                ? binding.layout.filesystem_blocks - begin : geometry.blocks_per_group);
            u32 free = 0;
            for (u32 local = 0; local < geometry.blocks_per_group; ++local)
            {
                const bool allocated = scratch.bitmap[local / 8] & (1U << (local % 8));
                if (local >= count || !Ext4FirstDataPlanner::ordinary_block(binding, begin + local))
                {
                    if (!allocated) return view.failure(Status::io_error("zero truncate ownership reserved bit clear"));
                }
                else if (!allocated) ++free;
            }
            if (free != le16(d, 12))
                return view.failure(Status::io_error("zero truncate ownership block free count"));
        }
        if (total_free_inodes != le32(scratch.superblock, 16))
            return view.failure(Status::io_error("zero truncate ownership total free inode count"));
        if (prepared.count)
        {
            prepared.group = (prepared.mapping[0] - 1) / geometry.blocks_per_group;
            for (u32 index = 0; index < prepared.count; ++index)
                if ((prepared.mapping[index] - 1) / geometry.blocks_per_group != prepared.group)
                    return Status::unsupported("zero truncate ownership release crosses groups");
        }
        std::array<u8, 256> final_inode{};
        if (auto status = view.decode_inode(update.inode, final_inode, scratch.global.block); !status)
            return view.failure(status);
        if (scratch.inode != final_inode)
            return view.failure(Status::io_error("zero truncate ownership target changed"));
        prepared.inode = update.inode; prepared.generation = update.generation; prepared.old_size = update.old_size;
        output = prepared;
        // Target completion has no intervening callback after this assignment.
        if constexpr (Capture)
            return capture_completed_target(view, *reader, binding, output, *birth, *progress, child);
        return {};
    }
};

struct Ext4AllocatedEmptyDataUpdate
{
    u32 inode{}, generation{}, size{}, seconds{}, nanoseconds{};
};
struct Ext4AllocatedEmptyDataSource
{
    u32 group{}, physical{}, inode_table_block{};
    const Ext4FirstDataBlock &superblock, &descriptors, &block_bitmap, &inode_bitmap, &inode_table;
};
struct Ext4AllocatedEmptyDataPlan
{
    Ext4AllocatedEmptyDataUpdate update{};
    u32 group{}, physical{};
    u64 old_version{}, new_version{};
    std::array<u32, 4> targets{};
    std::array<Ext4FirstDataBlock, 4> before{}, after{};
    usize count{};
};
struct Ext4AllocatedEmptyDataScratch
{
    Ext4ZeroTruncatePlan inspection{}, inspected{};
    Ext4AllocatedEmptyDataPlan prepared{};
};

// Pure planning for a checked allocated, linked empty inode. Preserved version,
// atime and creation time are not fresh-create provenance. The mounted owner
// still owes full allocation ownership, writer exclusion and ordered data I/O.
class Ext4AllocatedEmptyDataPlanner final
{
    using Parent = Ext4FirstDataPlanner;
    static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
  public:
    [[nodiscard]] static Status plan_off_stack(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataUpdate &update, const Ext4AllocatedEmptyDataSource &source,
        Ext4AllocatedEmptyDataScratch &scratch, Ext4AllocatedEmptyDataPlan &output)
    {
        using namespace ext4_checked;
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(output, binding) ||
            overlap(scratch, update) || overlap(output, update) ||
            overlap(scratch, source) || overlap(output, source))
            return Status::invalid_argument("allocated empty data workspace overlap");
        for (const auto *image : {&source.superblock, &source.descriptors, &source.block_bitmap,
                                  &source.inode_bitmap, &source.inode_table})
            if (overlap(scratch, *image) || overlap(output, *image))
                return Status::invalid_argument("allocated empty data image/workspace overlap");
        scratch = {}; output = {};
        if (!update.size || update.size > 1024 || update.seconds > 0x7fffffffU ||
            update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("allocated empty data size or timestamp");
        const Ext4ZeroTruncateSource empty_source{source.group, source.inode_table_block,
            source.superblock, source.descriptors, source.block_bitmap, source.inode_bitmap, source.inode_table};
        const Ext4ZeroTruncateUpdate identity{update.inode, update.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncatePlanner::plan_off_stack(binding, identity, empty_source,
                scratch.inspection, scratch.inspected); !status) return status;
        if (!scratch.inspected.noop || scratch.inspected.released_count || scratch.inspected.count)
            return Status::fault("allocated empty data source is not an exact empty inode");
        const auto selected = Parent::select_block(binding, source.superblock, source.descriptors,
            source.block_bitmap, source.group);
        if (!selected) return selected.status();
        if (source.physical != selected.value())
            return Status::invalid_argument("allocated empty data candidate is not canonical");
        Parent::Geometry geometry{};
        if (auto status = Parent::inspect(binding, source.superblock, source.descriptors, geometry);
            !status) return status;
        const usize offset = usize((update.inode - 1) % geometry.inodes_per_group % 4) * 256;
        const auto inode = std::span<const u8>{source.inode_table}.subspan(offset, 256);
        const u64 version = u64{le32(inode, 36)} | (u64{le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("allocated empty data inode version exhausted");
        const auto descriptor = std::span<const u8>{source.descriptors}.subspan(source.group * 32, 32);
        auto &prepared = scratch.prepared;
        prepared.update = update; prepared.group = source.group; prepared.physical = source.physical;
        prepared.old_version = version; prepared.new_version = version + 1;
        prepared.targets = {le32(descriptor, 0), 2, 1, source.inode_table_block};
        prepared.before[0] = source.block_bitmap;
        prepared.before[1] = source.descriptors;
        prepared.before[2] = source.superblock;
        prepared.before[3] = source.inode_table;
        prepared.after = prepared.before; prepared.count = 4;
        const u32 local = source.physical - (1 + source.group * geometry.blocks_per_group);
        prepared.after[0][local / 8] |= u8(1U << (local % 8));
        auto next_descriptor = std::span<u8>{prepared.after[1]}.subspan(source.group * 32, 32);
        Parent::store16(next_descriptor, 12, u16(le16(descriptor, 12) - 1));
        Parent::store16(next_descriptor, 24, u16(jbd2::detail::crc32c(geometry.seed,
            std::span<const u8>{prepared.after[0]}.first(geometry.blocks_per_group / 8))));
        Parent::store16(next_descriptor, 30, 0);
        Parent::store16(next_descriptor, 30, Parent::descriptor_checksum(geometry.seed, source.group, next_descriptor));
        Parent::store32(prepared.after[2], 12, le32(source.superblock, 12) - 1);
        Parent::store32(prepared.after[2], 1020, jbd2::detail::crc32c(~u32{0},
            std::span<const u8>{prepared.after[2]}.first(1020)));
        auto next_inode = std::span<u8>{prepared.after[3]}.subspan(offset, 256);
        Parent::store32(next_inode, 4, update.size);
        Parent::store32(next_inode, 12, update.seconds); Parent::store32(next_inode, 132, update.nanoseconds << 2);
        Parent::store32(next_inode, 16, update.seconds); Parent::store32(next_inode, 136, update.nanoseconds << 2);
        Parent::store32(next_inode, 28, 2);
        Parent::store32(next_inode, 36, u32(prepared.new_version));
        Parent::store32(next_inode, 152, u32(prepared.new_version >> 32));
        Parent::store16(next_inode, 42, 1); Parent::store16(next_inode, 56, 1);
        Parent::store32(next_inode, 60, source.physical);
        Parent::stamp_inode(next_inode, update.inode, geometry.seed);
        output = prepared;
        return {};
    }
};

struct Ext4ZeroTruncateWitness
{
    Ext4ZeroTruncateUpdate update{};
    std::array<u8, 16> uuid{};
    std::array<u32, 4> targets{};
    std::array<u32, 16> released_mapping{};
    u32 group{}, sequence{}, released_count{};
};
struct Ext4ZeroTruncateWitnessScratch
{
    Ext4ZeroTruncateBlock block{}, encoded{};
    Ext4ZeroTruncatePlan reconstructed{}, planner{}, canonical{};
};

// Private fixed-length descriptor padding protected by the descriptor CRC.
// The original inode is exact; bitmap and counter preimages are reconstructed
// by a checked inverse transform and then regenerated by the pure planner.
// Public image facts never grant journal, device, session or lifetime authority.
class Ext4ZeroTruncateWitnessCodec final
{
    using Block = Ext4ZeroTruncateBlock;
    using Parent = Ext4FirstDataPlanner;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'Z', 'T', 'W', '0', '0', '1'};
    static void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    [[nodiscard]] static bool overlaps(const void *one, usize one_size, const void *two, usize two_size)
    {
        const auto a = reinterpret_cast<usize>(one), b = reinterpret_cast<usize>(two);
        return a <= b ? b - a < one_size : a - b < two_size;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static bool same_plan(const Ext4ZeroTruncatePlan &a, const Ext4ZeroTruncatePlan &b)
    {
        return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size &&
            a.group == b.group && a.released_count == b.released_count &&
            a.released_mapping == b.released_mapping && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count && a.noop == b.noop;
    }
    [[nodiscard]] static Status regenerate(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateUpdate &update, const Block &inode_bitmap,
        Ext4ZeroTruncateWitnessScratch &scratch)
    {
        const auto &plan = scratch.reconstructed;
        const Ext4ZeroTruncateSource source{plan.group, plan.targets[0], plan.before[3],
            plan.before[2], plan.before[1], inode_bitmap, plan.before[0]};
        if (auto status = Ext4ZeroTruncatePlanner::plan_off_stack(binding, update, source,
                scratch.planner, scratch.canonical); !status) return status;
        return same_plan(plan, scratch.canonical) ? Status{} :
            Status::io_error("zero truncate witness is not the canonical four-home transform");
    }
    [[nodiscard]] static Status check_tags(const Ext4RecoveryInfo &binding,
        const Block &descriptor, std::span<const Block, 4> after,
        Ext4ZeroTruncateWitnessScratch &scratch)
    {
        using namespace ext4_checked;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{descriptor}.subspan(8, 4);
        for (usize at = 28; at < 44; ++at)
            if (descriptor[at]) return Status::io_error("zero truncate first tag UUID padding");
        for (usize at = 92; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("zero truncate tag padding");
        for (usize home = 0; home < 4; ++home)
        {
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            const u32 target = le32(descriptor, 188 + home * 4);
            const bool escaped = jbd2::detail::be32(after[home], 0) == jbd2::magic;
            const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
                (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0);
            if (!target || target >= binding.layout.filesystem_blocks ||
                jbd2::detail::be32(descriptor, at) != target ||
                jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8) ||
                le32(descriptor, 220 + home * 4) != jbd2::detail::crc32c(~u32{0}, after[home]))
                return Status::io_error("zero truncate whole after-home or tag binding");
            for (usize prior = 0; prior < home; ++prior)
                if (le32(descriptor, 188 + prior * 4) == target)
                    return Status::io_error("zero truncate duplicate home target");
            scratch.block = after[home];
            if (escaped) store32(scratch.block, 0, 0);
            if (jbd2::detail::be32(descriptor, at + 12) !=
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), scratch.block))
                return Status::io_error("zero truncate payload tag checksum");
        }
        return {};
    }

  public:
    static constexpr usize begin = 128, inode_begin = 304, length = 560, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize index = 0; index < magic.size(); ++index)
            if (descriptor[begin + index] != magic[index]) return false;
        return true;
    }
    // Caller-owned scratch keeps the canonical regeneration off the kernel
    // stack. Every error leaves descriptor unchanged. CRC stamping is a later
    // journal publication step, after this reversible extension is installed.
    [[nodiscard]] static Status encode(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncatePlan &plan, const Block &inode_bitmap, u32 sequence,
        Ext4ZeroTruncateWitnessScratch &scratch, Block &descriptor)
    {
        using namespace ext4_checked;
        if (overlap(scratch, plan) || overlap(scratch, binding) || overlap(scratch, inode_bitmap) ||
            overlap(scratch, descriptor) || overlap(descriptor, plan) ||
            overlap(descriptor, binding) || overlap(descriptor, inode_bitmap))
            return Status::invalid_argument("zero truncate witness encode workspace overlap");
        if (plan.count != 4 || plan.noop || !plan.released_count || plan.released_count > 16 ||
            !sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::invalid_argument("zero truncate witness plan or descriptor sequence");
        const u32 inodes_per_group = le32(plan.before[3], 40);
        if (!inodes_per_group || !plan.inode || plan.inode > le32(plan.before[3], 0))
            return Status::invalid_argument("zero truncate witness inode geometry");
        const usize offset = ((plan.inode - 1) % inodes_per_group % 4) * 256;
        const auto inode = std::span<const u8>{plan.after[0]}.subspan(offset, 256);
        const Ext4ZeroTruncateUpdate update{plan.inode, plan.generation, plan.old_size,
            le32(inode, 12), le32(inode, 132) >> 2};
        scratch.reconstructed = plan;
        if (auto status = regenerate(binding, update, inode_bitmap, scratch); !status) return status;
        scratch.encoded = descriptor;
        for (usize at = begin; at < end; ++at) scratch.encoded[at] = 0;
        for (usize index = 0; index < magic.size(); ++index) scratch.encoded[begin + index] = magic[index];
        store16(scratch.encoded, 136, 1); store16(scratch.encoded, 138, u16(length));
        scratch.encoded[140] = 4; scratch.encoded[141] = u8(plan.released_count);
        for (usize index = 0; index < binding.uuid.size(); ++index) scratch.encoded[144 + index] = binding.uuid[index];
        store32(scratch.encoded, 160, sequence); store32(scratch.encoded, 164, plan.inode);
        store32(scratch.encoded, 168, plan.generation); store32(scratch.encoded, 172, plan.old_size);
        store32(scratch.encoded, 176, plan.group); store32(scratch.encoded, 180, update.seconds);
        store32(scratch.encoded, 184, update.nanoseconds);
        for (usize home = 0; home < 4; ++home)
        {
            store32(scratch.encoded, 188 + home * 4, plan.targets[home]);
            store32(scratch.encoded, 204 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(scratch.encoded, 220 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.after[home]));
        }
        for (usize index = 0; index < 16; ++index)
            store32(scratch.encoded, 236 + index * 4, plan.released_mapping[index]);
        for (usize index = 0; index < 256; ++index)
            scratch.encoded[inode_begin + index] = plan.before[0][offset + index];
        // Tags must already describe the exact logical after images. The
        // descriptor CRC is intentionally not checked until decode.
        if (auto status = check_tags(binding, scratch.encoded, plan.after, scratch); !status) return status;
        descriptor = scratch.encoded;
        return {};
    }

    // Validates the fully checksummed descriptor and logical after images.
    // All output facts are cleared on non-alias errors. The unchanged inode
    // bitmap is supplied from the caller's exclusive checked view.
    [[nodiscard]] static Status decode(const Ext4RecoveryInfo &binding,
        const Block &descriptor, std::span<const Block, 4> after, const Block &inode_bitmap,
        Ext4ZeroTruncateWitnessScratch &scratch, Ext4ZeroTruncatePlan &output,
        Ext4ZeroTruncateWitness &witness)
    {
        using namespace ext4_checked;
        for (const auto *home : {&after[0], &after[1], &after[2], &after[3]})
            if (overlap(scratch, *home) || overlap(output, *home) || overlap(witness, *home))
                return Status::invalid_argument("zero truncate witness after/workspace overlap");
        if (overlap(scratch, output) || overlap(scratch, witness) || overlap(output, witness) ||
            overlap(scratch, binding) || overlap(scratch, descriptor) || overlap(scratch, inode_bitmap) ||
            overlap(output, binding) || overlap(output, descriptor) || overlap(output, inode_bitmap) ||
            overlap(witness, binding) || overlap(witness, descriptor) || overlap(witness, inode_bitmap))
            return Status::invalid_argument("zero truncate witness decode workspace overlap");
        scratch = {}; output = {}; witness = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("zero truncate witness absent or edition unknown");
        const u32 released = descriptor[141], sequence = le32(descriptor, 160);
        if (le16(descriptor, 138) != length || descriptor[140] != 4 || !released || released > 16 ||
            le16(descriptor, 142) || le32(descriptor, 300))
            return Status::io_error("zero truncate witness fixed-length or reserved fields");
        for (usize at = length; at < end; ++at)
            if (descriptor[at]) return Status::io_error("zero truncate witness trailing bytes");
        for (usize index = released; index < 16; ++index)
            if (le32(descriptor, 236 + index * 4))
                return Status::io_error("zero truncate witness unused mapping");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (descriptor[144 + index] != binding.uuid[index])
                return Status::io_error("zero truncate witness UUID");
        if (!sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::io_error("zero truncate witness descriptor sequence");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        scratch.block = descriptor;
        store32(scratch.block, 1020, 0);
        if (jbd2::detail::be32(descriptor, 1020) != jbd2::detail::crc32c(seed, scratch.block))
            return Status::io_error("zero truncate descriptor checksum");
        if (auto status = check_tags(binding, descriptor, after, scratch); !status) return status;
        auto &plan = scratch.reconstructed;
        plan.inode = le32(descriptor, 164); plan.generation = le32(descriptor, 168);
        plan.old_size = le32(descriptor, 172); plan.group = le32(descriptor, 176);
        plan.released_count = released; plan.count = 4;
        for (usize home = 0; home < 4; ++home)
        { plan.targets[home] = le32(descriptor, 188 + home * 4); plan.before[home] = plan.after[home] = after[home]; }
        Parent::Geometry geometry{};
        if (auto status = Parent::inspect(binding, after[3], after[2], geometry); !status) return status;
        if (plan.inode < geometry.first_inode || plan.inode > geometry.inodes || plan.group >= geometry.groups)
            return Status::io_error("zero truncate witnessed inode or data group");
        const u32 inode_group = (plan.inode - 1) / geometry.inodes_per_group;
        const u32 local = (plan.inode - 1) % geometry.inodes_per_group;
        const usize offset = (local % 4) * 256, data_at = plan.group * 32;
        if (plan.targets != std::array<u32, 4>{le32(after[2], inode_group * 32 + 8) + local / 4,
                le32(after[2], data_at), 2, 1})
            return Status::io_error("zero truncate witnessed home placement");
        for (usize index = 0; index < 256; ++index)
            plan.before[0][offset + index] = descriptor[inode_begin + index];
        const auto old_inode = std::span<const u8, 256>{plan.before[0].data() + offset, 256};
        if (le32(old_inode, 100) != plan.generation || le32(old_inode, 4) != plan.old_size)
            return Status::io_error("zero truncate original inode identity or size");
        u32 count{};
        if (auto status = Ext4ZeroTruncatePlanner::inspect_inode(binding, plan.inode, old_inode,
                plan.released_mapping, count); !status) return status;
        if (count != released) return Status::io_error("zero truncate original inode block count");
        const u32 begin_block = 1 + plan.group * geometry.blocks_per_group;
        for (u32 index = 0; index < count; ++index)
        {
            const u32 physical = plan.released_mapping[index];
            if (physical != le32(descriptor, 236 + index * 4) ||
                (physical - 1) / geometry.blocks_per_group != plan.group)
                return Status::io_error("zero truncate witnessed mapping or release group");
            const u32 bit = physical - begin_block;
            const u8 mask = u8(1U << (bit % 8));
            if (plan.before[1][bit / 8] & mask)
                return Status::io_error("zero truncate released block is allocated in after image");
            plan.before[1][bit / 8] |= mask;
        }
        const u16 free = le16(after[2], data_at + 12);
        const u32 total = le32(after[3], 12);
        if (free < released || total < released)
            return Status::io_error("zero truncate inverse free counter underflow");
        auto d = std::span<u8>{plan.before[2]}.subspan(data_at, 32);
        store16(d, 12, u16(free - released));
        store16(d, 24, u16(jbd2::detail::crc32c(seed,
            std::span<const u8>{plan.before[1]}.first(geometry.blocks_per_group / 8))));
        store16(d, 30, 0); store16(d, 30, Parent::descriptor_checksum(seed, plan.group, d));
        store32(plan.before[3], 12, total - released);
        store32(plan.before[3], 1020, jbd2::detail::crc32c(~u32{0},
            std::span<const u8>{plan.before[3]}.first(1020)));
        for (usize home = 0; home < 4; ++home)
            if (le32(descriptor, 204 + home * 4) != jbd2::detail::crc32c(~u32{0}, plan.before[home]))
                return Status::io_error("zero truncate whole before-home checksum");
        const Ext4ZeroTruncateUpdate update{plan.inode, plan.generation, plan.old_size,
            le32(descriptor, 180), le32(descriptor, 184)};
        if (auto status = regenerate(binding, update, inode_bitmap, scratch); !status) return status;
        output = plan;
        witness.update = update; witness.uuid = binding.uuid; witness.targets = plan.targets;
        witness.released_mapping = plan.released_mapping; witness.group = plan.group;
        witness.sequence = sequence; witness.released_count = released;
        return {};
    }
};

// Classification facts only. A caller must separately validate the complete
// committed journal and both allocation-ownership views before any recovery.
class Ext4ZeroTruncateHomeMatcher final
{
  public:
    [[nodiscard]] static Status classify(const Ext4ZeroTruncatePlan &plan,
        std::span<const Ext4ZeroTruncateBlock, 4> homes, u8 &after_mask)
    {
        const auto result = reinterpret_cast<usize>(&after_mask);
        const auto overlaps_result = [result](const void *input, usize size)
        {
            const auto begin = reinterpret_cast<usize>(input);
            return result >= begin && result - begin < size;
        };
        if (overlaps_result(&plan, sizeof(plan)) || overlaps_result(homes.data(), homes.size_bytes()))
            return Status::invalid_argument("zero truncate mixed-home result/input overlap");
        after_mask = 0;
        if (plan.count != 4 || plan.noop || !plan.released_count || plan.released_count > 16)
            return Status::invalid_argument("zero truncate mixed-home plan shape");
        u8 proposed{};
        for (usize home = 0; home < 4; ++home)
        {
            if (plan.before[home] == plan.after[home])
                return Status::invalid_argument("zero truncate mixed-home unchanged participant");
            if (homes[home] == plan.after[home]) proposed |= u8(1U << home);
            else if (homes[home] != plan.before[home])
                return Status::io_error("zero truncate home is neither exact old nor exact after");
        }
        after_mask = proposed;
        return {};
    }
};

struct Ext4AllocatedEmptyDataOwnership
{
    u32 inode{}, generation{}, group{}, physical{};
    u64 old_version{};
    bool complete{};
};
struct Ext4AllocatedEmptyDataOwnershipScratch
{
    Ext4ZeroTruncateOwnershipScratch empty{};
    Ext4ZeroTruncateOwnership inspected{};
    Ext4AllocatedEmptyDataScratch planner{};
    Ext4AllocatedEmptyDataPlan canonical{};
    Ext4AllocatedEmptyDataOwnership proposed{};
    Ext4FirstDataBlock bitmap{}, inode_bitmap{}, inode_table{}, bookend{};
};

// Read-only candidate facts over a retained exclusive view. The global proof
// checks all allocated records, reserved mappings and allocation counts. A free
// candidate therefore has no allocated reference. Exact source regeneration
// and final bookends bind these facts to the complete proposed old homes.
// The caller still owns exclusion, device/session lifetime and dispatch rights.
class Ext4AllocatedEmptyDataOwnershipValidator final
{
    static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
  public:
    template <jbd2::BlockReader Reader>
    [[nodiscard]] static Status prove(const Ext4RecoveryInfo &binding, Ext4ReadOnlyView<Reader> &view,
        const Ext4AllocatedEmptyDataPlan &plan, Ext4AllocatedEmptyDataOwnershipScratch &scratch,
        Ext4AllocatedEmptyDataOwnership &output)
    {
        using namespace ext4_checked;
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(scratch, view) ||
            overlap(scratch, plan) || overlap(output, binding) || overlap(output, view) || overlap(output, plan))
            return Status::invalid_argument("allocated empty ownership input/workspace overlap");
        if (view.reader_ && (overlap(scratch, *view.reader_) || overlap(output, *view.reader_)))
            return Status::invalid_argument("allocated empty ownership retained reader/workspace overlap");
        scratch = {}; output = {};
        if (plan.count != 4) return Status::invalid_argument("allocated empty ownership plan shape");
        const Ext4ZeroTruncateUpdate identity{plan.update.inode, plan.update.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(binding, view, identity,
                scratch.empty, scratch.inspected); !status) return status;
        if (scratch.inspected.count || scratch.inspected.old_size)
            return Status::io_error("allocated empty ownership target is not empty");
        const u32 groups = u32(binding.metadata_count / 4);
        const u32 per_group = le32(scratch.empty.superblock, 40);
        if (!per_group || plan.group >= groups)
            return Status::invalid_argument("allocated empty ownership candidate group");
        const u32 inode_group = (plan.update.inode - 1) / per_group;
        if (inode_group >= groups) return Status::invalid_argument("allocated empty ownership inode group");
        const usize data_at = usize(plan.group) * 32, inode_at = usize(inode_group) * 32;
        const u32 bitmap = le32(scratch.empty.descriptors, data_at);
        const u32 inode_bitmap = le32(scratch.empty.descriptors, inode_at + 4);
        const u32 local = (plan.update.inode - 1) % per_group;
        const u32 table = le32(scratch.empty.descriptors, inode_at + 8) + local / 4;
        if (auto status = view.block(bitmap, scratch.bitmap); !status) return status;
        if (auto status = view.block(inode_bitmap, scratch.inode_bitmap); !status) return status;
        if (auto status = view.block(table, scratch.inode_table); !status) return status;
        for (usize at = 0; at < 256; ++at)
            if (scratch.inode_table[(local % 4) * 256 + at] != scratch.empty.inode[at])
                return view.failure(Status::io_error("allocated empty ownership target changed after global proof"));
        const Ext4AllocatedEmptyDataSource source{plan.group, plan.physical, table,
            scratch.empty.superblock, scratch.empty.descriptors, scratch.bitmap,
            scratch.inode_bitmap, scratch.inode_table};
        if (auto status = Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding, plan.update,
                source, scratch.planner, scratch.canonical); !status) return status;
        const auto &canonical = scratch.canonical;
        if (canonical.count != plan.count || canonical.group != plan.group || canonical.physical != plan.physical ||
            canonical.old_version != plan.old_version || canonical.new_version != plan.new_version ||
            canonical.targets != plan.targets || canonical.before != plan.before || canonical.after != plan.after)
            return Status::io_error("allocated empty ownership plan differs from exclusive source");
        const std::array<u32, 5> numbers{1, 2, bitmap, inode_bitmap, table};
        const std::array<const Ext4FirstDataBlock *, 5> images{&scratch.empty.superblock,
            &scratch.empty.descriptors, &scratch.bitmap, &scratch.inode_bitmap, &scratch.inode_table};
        for (usize index = 0; index < numbers.size(); ++index)
        {
            if (auto status = view.block(numbers[index], scratch.bookend); !status) return status;
            if (scratch.bookend != *images[index])
                return view.failure(Status::io_error("allocated empty ownership final source bookend changed"));
        }
        scratch.proposed = {plan.update.inode, plan.update.generation, plan.group, plan.physical, plan.old_version, true};
        output = scratch.proposed;
        return {};
    }
};

struct Ext4AllocatedEmptyDataWitness
{
    Ext4AllocatedEmptyDataUpdate update{};
    std::array<u8, 16> uuid{};
    std::array<u32, 4> targets{};
    u32 group{}, physical{}, sequence{};
    u64 old_version{}, new_version{};
};
struct Ext4AllocatedEmptyDataWitnessScratch
{
    Ext4FirstDataBlock block{}, encoded{};
    Ext4AllocatedEmptyDataPlan reconstructed{}, canonical{};
    Ext4AllocatedEmptyDataScratch planner{};
};

// Image facts only. Exact old-inode bytes and checked inverse allocation
// reconstruct the four old homes; the pure planner must regenerate every byte.
// Neither this descriptor extension nor CRCs grant transport or recovery rights.
class Ext4AllocatedEmptyDataWitnessCodec final
{
    using Block = Ext4FirstDataBlock;
    using Parent = Ext4FirstDataPlanner;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'A', 'E', 'W', '0', '0', '1'};
    static void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    static bool same_plan(const Ext4AllocatedEmptyDataPlan &a, const Ext4AllocatedEmptyDataPlan &b)
    {
        return a.update.inode == b.update.inode && a.update.generation == b.update.generation &&
            a.update.size == b.update.size && a.update.seconds == b.update.seconds &&
            a.update.nanoseconds == b.update.nanoseconds && a.group == b.group &&
            a.physical == b.physical && a.old_version == b.old_version && a.new_version == b.new_version &&
            a.targets == b.targets && a.before == b.before && a.after == b.after && a.count == b.count;
    }
    static Status regenerate(const Ext4RecoveryInfo &binding, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataWitnessScratch &scratch)
    {
        const auto &p = scratch.reconstructed;
        const Ext4AllocatedEmptyDataSource source{p.group, p.physical, p.targets[3],
            p.before[2], p.before[1], p.before[0], inode_bitmap, p.before[3]};
        if (auto status = Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding, p.update,
                source, scratch.planner, scratch.canonical); !status) return status;
        return same_plan(p, scratch.canonical) ? Status{} :
            Status::io_error("allocated empty witness is not the canonical four-home transform");
    }
    static Status check_tags(const Ext4RecoveryInfo &binding, const Block &descriptor,
        std::span<const Block, 4> after, Ext4AllocatedEmptyDataWitnessScratch &scratch)
    {
        using namespace ext4_checked;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{descriptor}.subspan(8, 4);
        for (usize at = 28; at < 44; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty first tag UUID padding");
        for (usize at = 92; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty tag padding");
        for (usize home = 0; home < 4; ++home)
        {
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            const u32 target = le32(descriptor, 192 + home * 4);
            const bool escaped = jbd2::detail::be32(after[home], 0) == jbd2::magic;
            const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
                (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0);
            if (!target || target >= binding.layout.filesystem_blocks ||
                jbd2::detail::be32(descriptor, at) != target ||
                jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8) ||
                le32(descriptor, 224 + home * 4) != jbd2::detail::crc32c(~u32{0}, after[home]))
                return Status::io_error("allocated empty whole after-home or tag binding");
            for (usize prior = 0; prior < home; ++prior)
                if (le32(descriptor, 192 + prior * 4) == target)
                    return Status::io_error("allocated empty duplicate home target");
            scratch.block = after[home];
            if (escaped) store32(scratch.block, 0, 0);
            if (jbd2::detail::be32(descriptor, at + 12) !=
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), scratch.block))
                return Status::io_error("allocated empty payload tag checksum");
        }
        return {};
    }
  public:
    static constexpr usize begin = 128, inode_begin = 240, length = 496, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize index = 0; index < magic.size(); ++index)
            if (descriptor[begin + index] != magic[index]) return false;
        return true;
    }
    // Every failure preserves descriptor. Its final CRC is stamped by the
    // journal formatter after the extension is installed in valid tags.
    [[nodiscard]] static Status encode(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataPlan &plan, const Block &inode_bitmap, u32 sequence,
        Ext4AllocatedEmptyDataWitnessScratch &scratch, Block &descriptor)
    {
        using namespace ext4_checked;
        if (overlap(scratch, plan) || overlap(scratch, binding) || overlap(scratch, inode_bitmap) ||
            overlap(scratch, descriptor) || overlap(descriptor, plan) ||
            overlap(descriptor, binding) || overlap(descriptor, inode_bitmap))
            return Status::invalid_argument("allocated empty witness encode workspace overlap");
        if (plan.count != 4 || !sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::invalid_argument("allocated empty witness plan or descriptor sequence");
        scratch.reconstructed = plan;
        if (auto status = regenerate(binding, inode_bitmap, scratch); !status) return status;
        const u32 per_group = le32(plan.before[2], 40);
        const usize offset = usize((plan.update.inode - 1) % per_group % 4) * 256;
        scratch.encoded = descriptor;
        for (usize at = begin; at < end; ++at) scratch.encoded[at] = 0;
        for (usize index = 0; index < magic.size(); ++index) scratch.encoded[begin + index] = magic[index];
        store16(scratch.encoded, 136, 1); store16(scratch.encoded, 138, u16(length));
        scratch.encoded[140] = 4;
        for (usize index = 0; index < binding.uuid.size(); ++index) scratch.encoded[144 + index] = binding.uuid[index];
        store32(scratch.encoded, 160, sequence); store32(scratch.encoded, 164, plan.update.inode);
        store32(scratch.encoded, 168, plan.update.generation); store32(scratch.encoded, 172, plan.update.size);
        store32(scratch.encoded, 176, plan.group); store32(scratch.encoded, 180, plan.physical);
        store32(scratch.encoded, 184, plan.update.seconds); store32(scratch.encoded, 188, plan.update.nanoseconds);
        for (usize home = 0; home < 4; ++home)
        {
            store32(scratch.encoded, 192 + home * 4, plan.targets[home]);
            store32(scratch.encoded, 208 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(scratch.encoded, 224 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.after[home]));
        }
        for (usize index = 0; index < 256; ++index)
            scratch.encoded[inode_begin + index] = plan.before[3][offset + index];
        if (auto status = check_tags(binding, scratch.encoded, plan.after, scratch); !status) return status;
        descriptor = scratch.encoded;
        return {};
    }
    // Requires logical unescaped after images and a separately retained,
    // checked inode bitmap. Non-alias failures clear both output fact records.
    [[nodiscard]] static Status decode(const Ext4RecoveryInfo &binding, const Block &descriptor,
        std::span<const Block, 4> after, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataWitnessScratch &scratch, Ext4AllocatedEmptyDataPlan &output,
        Ext4AllocatedEmptyDataWitness &witness)
    {
        using namespace ext4_checked;
        for (const auto *home : {&after[0], &after[1], &after[2], &after[3]})
            if (overlap(scratch, *home) || overlap(output, *home) || overlap(witness, *home))
                return Status::invalid_argument("allocated empty witness after/workspace overlap");
        if (overlap(scratch, output) || overlap(scratch, witness) || overlap(output, witness) ||
            overlap(scratch, binding) || overlap(scratch, descriptor) || overlap(scratch, inode_bitmap) ||
            overlap(output, binding) || overlap(output, descriptor) || overlap(output, inode_bitmap) ||
            overlap(witness, binding) || overlap(witness, descriptor) || overlap(witness, inode_bitmap))
            return Status::invalid_argument("allocated empty witness decode workspace overlap");
        scratch = {}; output = {}; witness = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("allocated empty witness absent or edition unknown");
        if (le16(descriptor, 138) != length || descriptor[140] != 4 || descriptor[141] || le16(descriptor, 142))
            return Status::io_error("allocated empty witness fixed-length or reserved fields");
        for (usize at = length; at < end; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty witness trailing bytes");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (descriptor[144 + index] != binding.uuid[index])
                return Status::io_error("allocated empty witness UUID");
        const u32 sequence = le32(descriptor, 160);
        if (!sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::io_error("allocated empty witness descriptor sequence");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        scratch.block = descriptor; store32(scratch.block, 1020, 0);
        if (jbd2::detail::be32(descriptor, 1020) != jbd2::detail::crc32c(seed, scratch.block))
            return Status::io_error("allocated empty descriptor checksum");
        if (auto status = check_tags(binding, descriptor, after, scratch); !status) return status;
        auto &p = scratch.reconstructed;
        p.update = {le32(descriptor, 164), le32(descriptor, 168), le32(descriptor, 172),
            le32(descriptor, 184), le32(descriptor, 188)};
        p.group = le32(descriptor, 176); p.physical = le32(descriptor, 180); p.count = 4;
        for (usize home = 0; home < 4; ++home)
        { p.targets[home] = le32(descriptor, 192 + home * 4); p.before[home] = p.after[home] = after[home]; }
        Parent::Geometry geometry{};
        if (auto status = Parent::inspect(binding, after[2], after[1], geometry); !status) return status;
        if (p.update.inode < geometry.first_inode || p.update.inode > geometry.inodes ||
            p.group >= geometry.groups || !Parent::ordinary_block(binding, p.physical) ||
            (p.physical - 1) / geometry.blocks_per_group != p.group)
            return Status::io_error("allocated empty witnessed inode or data group");
        const u32 inode_group = (p.update.inode - 1) / geometry.inodes_per_group;
        const u32 local = (p.update.inode - 1) % geometry.inodes_per_group;
        const usize offset = usize(local % 4) * 256, data_at = usize(p.group) * 32;
        if (p.targets != std::array<u32, 4>{le32(after[1], data_at), 2, 1,
                le32(after[1], inode_group * 32 + 8) + local / 4})
            return Status::io_error("allocated empty witnessed home placement");
        for (usize index = 0; index < 256; ++index)
            p.before[3][offset + index] = descriptor[inode_begin + index];
        const auto old_inode = std::span<const u8>{p.before[3]}.subspan(offset, 256);
        p.old_version = u64{le32(old_inode, 36)} | (u64{le32(old_inode, 152)} << 32);
        if (p.old_version == ~u64{0}) return Status::overflow("allocated empty witnessed version exhausted");
        p.new_version = p.old_version + 1;
        const u32 bit = (p.physical - 1) % geometry.blocks_per_group;
        const u8 mask = u8(1U << (bit % 8));
        if (!(p.before[0][bit / 8] & mask))
            return Status::io_error("allocated empty candidate is free in after image");
        p.before[0][bit / 8] &= u8(~mask);
        const u16 free = le16(after[1], data_at + 12);
        const u32 total = le32(after[2], 12);
        if (free == 0xffffU || total == 0xffffffffU)
            return Status::overflow("allocated empty inverse free counter exhausted");
        auto d = std::span<u8>{p.before[1]}.subspan(data_at, 32);
        store16(d, 12, u16(free + 1));
        store16(d, 24, u16(jbd2::detail::crc32c(seed,
            std::span<const u8>{p.before[0]}.first(geometry.blocks_per_group / 8))));
        store16(d, 30, 0); store16(d, 30, Parent::descriptor_checksum(seed, p.group, d));
        store32(p.before[2], 12, total + 1);
        store32(p.before[2], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{p.before[2]}.first(1020)));
        for (usize home = 0; home < 4; ++home)
            if (le32(descriptor, 208 + home * 4) != jbd2::detail::crc32c(~u32{0}, p.before[home]))
                return Status::io_error("allocated empty whole before-home checksum");
        if (auto status = regenerate(binding, inode_bitmap, scratch); !status) return status;
        output = p; witness.update = p.update; witness.uuid = binding.uuid; witness.targets = p.targets;
        witness.group = p.group; witness.physical = p.physical; witness.sequence = sequence;
        witness.old_version = p.old_version; witness.new_version = p.new_version;
        return {};
    }
};

// Exact mixed-home classification is a fact, independent of journal commit,
// ownership proof, writer exclusion and any eventual replay authorization.
class Ext4AllocatedEmptyDataHomeMatcher final
{
  public:
    [[nodiscard]] static Status classify(const Ext4AllocatedEmptyDataPlan &plan,
        std::span<const Ext4FirstDataBlock, 4> homes, u8 &after_mask)
    {
        const auto result = reinterpret_cast<usize>(&after_mask);
        const auto overlaps_result = [result](const void *input, usize size)
        { const auto begin = reinterpret_cast<usize>(input); return result >= begin && result - begin < size; };
        if (overlaps_result(&plan, sizeof(plan)) || overlaps_result(homes.data(), homes.size_bytes()))
            return Status::invalid_argument("allocated empty mixed-home result/input overlap");
        after_mask = 0;
        if (plan.count != 4 || !plan.update.size || plan.update.size > 1024 ||
            plan.old_version == ~u64{0} || plan.new_version != plan.old_version + 1)
            return Status::invalid_argument("allocated empty mixed-home plan shape");
        u8 proposed{};
        for (usize home = 0; home < 4; ++home)
        {
            if (plan.before[home] == plan.after[home])
                return Status::invalid_argument("allocated empty mixed-home unchanged participant");
            if (homes[home] == plan.after[home]) proposed |= u8(1U << home);
            else if (homes[home] != plan.before[home])
                return Status::io_error("allocated empty home is neither exact old nor exact after");
        }
        after_mask = proposed;
        return {};
    }
};

struct Ext4ZeroTruncateJournalRecords
{
    const Ext4ZeroTruncateBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4ZeroTruncateBlock> payloads{};
};
struct Ext4ZeroTruncateJournalScratch
{
    Ext4ZeroTruncateWitnessScratch witness{};
    Ext4ZeroTruncatePlan decoded{};
    std::array<Ext4ZeroTruncateBlock, 4> after{};
    std::array<jbd2::ReplayAction, 4> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4ZeroTruncateBlock record{}, payload{};
};
struct Ext4ZeroTruncateJournalResult
{
    Ext4ZeroTruncateWitness witness{};
    u32 active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool complete{};
};

// Complete bounded journal and exact participant-image facts. These borrowing
// interfaces never authenticate a device snapshot or grant write authority.
class Ext4ZeroTruncateJournalVerifier final
{
    template <jbd2::BlockReader> friend class Ext4ZeroTruncatePreview;
    using Block = Ext4ZeroTruncateBlock;
    struct Reader
    {
        const Ext4ZeroTruncateJournalRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("zero truncate journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < 6) image = &records.payloads[number - 2];
            else if (number == 6) image = &records.commit;
            else if (number == 7) image = &records.end;
            if (!image) return Status::io_error("zero truncate journal fixed record bounds");
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = (*image)[byte];
            return {};
        }
    };
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] static bool overlaps(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static Status parse(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateJournalRecords &records, Ext4ZeroTruncateJournalScratch &scratch)
    {
        if (!binding.needs_recovery || binding.empty_log || records.payloads.size() != 4 ||
            binding.layout.block_size != 1024 ||
            !binding.structural_metadata().valid())
            return Status::invalid_argument("zero truncate active journal profile");
        if (!jbd2::detail::all_zero(records.end))
            return Status::io_error("zero truncate explicit end record is not zero");
        const auto opened = jbd2::Codec::open(records.head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!opened) return opened.status();
        const auto &head = opened.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.max_blocks != binding.layout.journal_blocks || head.max_blocks <= 7 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("zero truncate active sequence or fixed journal ring");
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(opened.value(), reader, binding.structural_metadata(),
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &plan = parsed.value();
        if (plan.empty_log || plan.incomplete_tail || plan.committed_transactions != 1 ||
            plan.revoked_actions || plan.revoke_records || plan.actions.size() != 4 || plan.next_sequence != head.sequence + 1)
            return Status::io_error("zero truncate complete committed journal grammar");
        for (usize home = 0; home < 4; ++home)
        {
            const auto &action = plan.actions[home];
            if (action.sequence != head.sequence || action.transaction_order || action.journal_block != home + 2 ||
                action.target_block != ext4_checked::le32(records.descriptor, 188 + home * 4))
                return Status::io_error("zero truncate exact journal action identity");
            scratch.after[home] = records.payloads[home];
            if (action.restore_magic) be32(scratch.after[home], 0, jbd2::magic);
        }
        return {};
    }
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateJournalRecords &records, const Block &inode_bitmap,
        std::span<const Block> actual_homes, Ext4ZeroTruncateJournalScratch &scratch,
        Ext4ZeroTruncateJournalResult &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(output, binding) ||
            overlap(scratch, records) || overlap(output, records) || overlap(scratch, inode_bitmap) ||
            overlap(output, inode_bitmap))
            return Status::invalid_argument("zero truncate journal request/workspace overlap");
        for (const auto *block : {&records.head, &records.descriptor, &records.commit, &records.end})
            if (overlap(scratch, *block) || overlap(output, *block))
                return Status::invalid_argument("zero truncate journal record/workspace overlap");
        for (const auto images : {records.payloads, actual_homes})
            if (overlaps(&scratch, sizeof(scratch), images.data(), images.size_bytes()) ||
                overlaps(&output, sizeof(output), images.data(), images.size_bytes()))
                return Status::invalid_argument("zero truncate journal image/workspace overlap");
        scratch = {}; output = {};
        if (actual_homes.size() != 4)
            return Status::invalid_argument("zero truncate actual whole-home count");
        if (auto status = parse(binding, records, scratch); !status) return status;
        Ext4ZeroTruncateWitness witness{};
        if (auto status = Ext4ZeroTruncateWitnessCodec::decode(binding, records.descriptor, scratch.after,
                inode_bitmap, scratch.witness, scratch.decoded, witness); !status) return status;
        if (witness.sequence != binding.journal_sequence)
            return Status::io_error("zero truncate witness active sequence differs");
        // A committed timestamp must describe this exact transform. Require the
        // private synchronous commit profile, including every unused byte.
        scratch.record = {};
        be32(scratch.record, 0, jbd2::magic); be32(scratch.record, 4, u32(jbd2::RecordType::commit));
        be32(scratch.record, 8, witness.sequence);
        be32(scratch.record, 52, witness.update.seconds); be32(scratch.record, 56, witness.update.nanoseconds);
        be32(scratch.record, 16, jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), scratch.record));
        if (scratch.record != records.commit)
            return Status::io_error("zero truncate exact commit image differs");
        u8 mask{};
        if (auto status = Ext4ZeroTruncateHomeMatcher::classify(scratch.decoded,
                std::span<const Block, 4>{actual_homes.data(), 4}, mask); !status) return status;
        output.witness = witness; output.active_sequence = witness.sequence;
        output.next_sequence = witness.sequence + 1; output.after_mask = mask; output.complete = true;
        return {};
    }
};

template <jbd2::BlockReader Reader> class Ext4ZeroTruncatePreview;

// Permanent one-shot scratch claim. This storage and its supplied transport
// must outlive the preview and remain exclusive; allocate outside kernel stacks.
template <jbd2::BlockReader Reader>
class Ext4ZeroTruncatePreviewWorkspace final
{
    friend class Ext4ZeroTruncatePreview<Reader>;
    template <typename> friend class Ext4ZeroTruncateTransaction;
    using Block = Ext4ZeroTruncateBlock;
    struct Overlay
    {
        Ext4ZeroTruncatePreviewWorkspace *workspace;
        bool final;
        Status read_block(u32 number, std::span<u8> out)
        {
            if (!workspace->journal_ready_ || out.size() < 1024)
                return Status::not_initialized("zero truncate ownership overlay is unavailable");
            for (usize home = 0; home < 4; ++home)
                if (number == workspace->journal_.decoded.targets[home])
                {
                    const auto &bytes = final ? workspace->journal_.decoded.after[home] : workspace->journal_.decoded.before[home];
                    for (usize byte = 0; byte < 1024; ++byte) out[byte] = bytes[byte];
                    return {};
                }
            return workspace->read(number, out.first(1024));
        }
    };
  public:
    Ext4ZeroTruncatePreviewWorkspace() = default;
    Ext4ZeroTruncatePreviewWorkspace(const Ext4ZeroTruncatePreviewWorkspace &) = delete;
    Ext4ZeroTruncatePreviewWorkspace &operator=(const Ext4ZeroTruncatePreviewWorkspace &) = delete;
    Ext4ZeroTruncatePreviewWorkspace(Ext4ZeroTruncatePreviewWorkspace &&) = delete;
    Ext4ZeroTruncatePreviewWorkspace &operator=(Ext4ZeroTruncatePreviewWorkspace &&) = delete;
  private:
    Status read(u32 number, std::span<u8> out)
    {
        if (!claimant_ || !reader_ || out.size() != 1024 || number >= binding_.layout.filesystem_blocks)
            return Status::invalid_argument("zero truncate preview transport read bounds");
        if (read_attempts_ == ~usize{0}) return Status::overflow("zero truncate preview read counter exhausted");
        ++read_attempts_;
        if (auto status = reader_->read_block(number, out); !status) return status;
        ++completed_reads_; return {};
    }
    const void *claimant_{};
    Reader *reader_{};
    Ext4RecoveryInfo binding_{};
    usize read_attempts_{}, completed_reads_{};
    u32 inode_bitmap_target_{};
    bool journal_ready_{};
    Block head_{}, descriptor_{}, commit_{}, end_{}, inode_bitmap_{}, checked_{};
    std::array<Block, 4> payloads_{}, homes_{};
    Ext4ZeroTruncateJournalScratch journal_{};
    Ext4ZeroTruncateJournalResult result_{};
    Ext4ZeroTruncateOwnershipScratch ownership_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4ZeroTruncateOwnership old_proof_{}, final_proof_{};
    Overlay old_reader_{this, false}, final_reader_{this, true};
    Ext4ReadOnlyView<Overlay> old_view_{}, final_view_{};
};

enum class Ext4ZeroTruncatePreviewPhase : u8 { unused, capturing, journal, ownership, readback, ready, failed };
struct Ext4ZeroTruncatePreviewReport
{
    Ext4ZeroTruncatePreviewPhase phase{Ext4ZeroTruncatePreviewPhase::unused};
    Status status{};
    usize read_attempts{}, completed_reads{};
    u32 inode{}, generation{}, released_count{}, active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool old_ownership{}, final_ownership{}, snapshot_readback{};
};

// Read-only typed preview from an exclusive caller-protected transport. It
// captures the complete journal and homes, proves full old/final ownership via
// exact overlays, then rechecks captured bytes. No Session or replay permission
// is manufactured. A canonical mounted owner and counter reservation are later.
template <jbd2::BlockReader Reader>
class Ext4ZeroTruncatePreview final
{
    template <typename> friend class Ext4ZeroTruncateTransaction;
    using Workspace = Ext4ZeroTruncatePreviewWorkspace<Reader>;
    using Block = Ext4ZeroTruncateBlock;
  public:
    Ext4ZeroTruncatePreview() = default;
    Ext4ZeroTruncatePreview(const Ext4ZeroTruncatePreview &) = delete;
    Ext4ZeroTruncatePreview &operator=(const Ext4ZeroTruncatePreview &) = delete;
    Ext4ZeroTruncatePreview(Ext4ZeroTruncatePreview &&) = delete;
    Ext4ZeroTruncatePreview &operator=(Ext4ZeroTruncatePreview &&) = delete;
    [[nodiscard]] const Ext4ZeroTruncatePreviewReport &report() const { return report_; }
    [[nodiscard]] Status prepare(Reader &reader, const Ext4RecoveryInfo &binding, Workspace &workspace)
    {
        if (report_.phase != Ext4ZeroTruncatePreviewPhase::unused)
            return Status::already_exists("zero truncate preview is one-shot");
        report_.phase = Ext4ZeroTruncatePreviewPhase::capturing;
        const auto overlap = [](const void *a, usize a_size, const void *b, usize b_size)
        {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < a_size : one - two < b_size;
        };
        if (overlap(this, sizeof(*this), &workspace, sizeof(workspace)) ||
            overlap(&binding, sizeof(binding), &workspace, sizeof(workspace)) ||
            overlap(&reader, sizeof(reader), &workspace, sizeof(workspace)))
            return fail(Status::invalid_argument("zero truncate preview Owner/workspace overlap"));
        if (workspace.claimant_) return fail(Status::already_exists("zero truncate preview workspace already claimed"));
        workspace.claimant_ = this; workspace_ = &workspace;
        workspace.reader_ = &reader; workspace.binding_ = binding;
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks > binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return fail(Status::invalid_argument("zero truncate preview bounded active layout"));
        const u32 first = u32(binding.layout.journal_first_block);
        for (usize index = 0; index < 8; ++index)
            if (auto status = workspace.read(first + u32(index), record(index)); !status) return fail(status);
        report_.phase = Ext4ZeroTruncatePreviewPhase::journal;
        const Ext4ZeroTruncateJournalRecords records{workspace.head_, workspace.descriptor_, workspace.commit_, workspace.end_, workspace.payloads_};
        if (auto status = Ext4ZeroTruncateJournalVerifier::parse(workspace.binding_, records, workspace.journal_); !status)
            return fail(status);
        const u32 inode = ext4_checked::le32(workspace.descriptor_, 164);
        const u32 per_group = ext4_checked::le32(workspace.journal_.after[3], 40);
        if (!per_group || !inode || inode > ext4_checked::le32(workspace.journal_.after[3], 0) ||
            (inode - 1) / per_group >= binding.metadata_count / 4)
            return fail(Status::io_error("zero truncate preview captured inode geometry"));
        const auto bitmap = binding.metadata[((inode - 1) / per_group) * 4 + 2];
        if (bitmap.count != 1 || bitmap.first > 0xffffffffULL)
            return fail(Status::io_error("zero truncate preview inode bitmap binding"));
        workspace.inode_bitmap_target_ = u32(bitmap.first);
        if (auto status = workspace.read(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status)
            return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = workspace.read(u32(workspace.journal_.actions[home].target_block), workspace.homes_[home]); !status)
                return fail(status);
        if (auto status = Ext4ZeroTruncateJournalVerifier::verify(workspace.binding_, records,
                workspace.inode_bitmap_, workspace.homes_, workspace.journal_, workspace.result_); !status) return fail(status);
        workspace.journal_ready_ = true;
        report_.phase = Ext4ZeroTruncatePreviewPhase::ownership;
        if (auto status = workspace.old_view_.open(workspace.old_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.old_view_,
                workspace.result_.witness.update, workspace.ownership_, workspace.old_proof_); !status) return fail(status);
        const auto &plan = workspace.journal_.decoded;
        if (workspace.old_proof_.mapping != plan.released_mapping || workspace.old_proof_.count != plan.released_count ||
            workspace.old_proof_.group != plan.group)
            return fail(Status::io_error("zero truncate old overlay mapping differs"));
        report_.old_ownership = true;
        if (auto status = workspace.final_view_.open(workspace.final_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        const Ext4ZeroTruncateUpdate empty{plan.inode, plan.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.final_view_,
                empty, workspace.ownership_, workspace.final_proof_); !status) return fail(status);
        if (workspace.final_proof_.count || workspace.final_proof_.inode != plan.inode ||
            workspace.final_proof_.generation != plan.generation)
            return fail(Status::io_error("zero truncate final overlay identity differs"));
        report_.final_ownership = true; report_.phase = Ext4ZeroTruncatePreviewPhase::readback;
        for (usize index = 0; index < 8; ++index)
            if (auto status = check(first + u32(index), record(index)); !status) return fail(status);
        if (auto status = check(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status) return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = check(plan.targets[home], workspace.homes_[home]); !status) return fail(status);
        report_.snapshot_readback = true;
        report_.inode = plan.inode; report_.generation = plan.generation; report_.released_count = plan.released_count;
        report_.active_sequence = workspace.result_.active_sequence; report_.next_sequence = workspace.result_.next_sequence;
        report_.after_mask = workspace.result_.after_mask;
        report_.phase = Ext4ZeroTruncatePreviewPhase::ready; update_counts(); return {};
    }
  private:
    Block &record(usize index)
    {
        if (!index) return workspace_->head_;
        if (index == 1) return workspace_->descriptor_;
        if (index < 6) return workspace_->payloads_[index - 2];
        return index == 6 ? workspace_->commit_ : workspace_->end_;
    }
    [[nodiscard]] Status check(u32 number, const Block &expected)
    {
        if (auto status = workspace_->read(number, workspace_->checked_); !status) return status;
        return workspace_->checked_ == expected ? Status{} : Status::io_error("zero truncate captured source changed during preview");
    }
    void update_counts()
    {
        if (workspace_)
        { report_.read_attempts = workspace_->read_attempts_; report_.completed_reads = workspace_->completed_reads_; }
    }
    [[nodiscard]] Status fail(Status status)
    { report_.phase = Ext4ZeroTruncatePreviewPhase::failed; report_.status = status; update_counts(); return status; }
    Workspace *workspace_{};
    Ext4ZeroTruncatePreviewReport report_{};
};

struct Ext4AllocatedEmptyDataJournalRecords
{
    const Ext4FirstDataBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4FirstDataBlock> payloads{};
};
struct Ext4AllocatedEmptyDataJournalScratch
{
    Ext4AllocatedEmptyDataWitnessScratch witness{};
    Ext4AllocatedEmptyDataPlan decoded{};
    std::array<Ext4FirstDataBlock, 4> after{};
    std::array<jbd2::ReplayAction, 4> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4FirstDataBlock record{}, payload{};
};
struct Ext4AllocatedEmptyDataJournalResult
{
    Ext4AllocatedEmptyDataWitness witness{};
    u32 active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool complete{};
};

// Complete bounded journal and exact participant-image facts. These borrowing
// interfaces never authenticate a device snapshot or grant write authority.
class Ext4AllocatedEmptyDataJournalVerifier final
{
    template <jbd2::BlockReader> friend class Ext4AllocatedEmptyDataPreview;
    using Block = Ext4FirstDataBlock;
    struct Reader
    {
        const Ext4AllocatedEmptyDataJournalRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("allocated empty journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < 6) image = &records.payloads[number - 2];
            else if (number == 6) image = &records.commit;
            else if (number == 7) image = &records.end;
            if (!image) return Status::io_error("allocated empty journal fixed record bounds");
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = (*image)[byte];
            return {};
        }
    };
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] static bool overlaps(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static Status parse(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataJournalRecords &records, Ext4AllocatedEmptyDataJournalScratch &scratch)
    {
        if (!binding.needs_recovery || binding.empty_log || records.payloads.size() != 4 ||
            binding.layout.block_size != 1024 ||
            !binding.structural_metadata().valid())
            return Status::invalid_argument("allocated empty active journal profile");
        if (!jbd2::detail::all_zero(records.end))
            return Status::io_error("allocated empty explicit end record is not zero");
        const auto opened = jbd2::Codec::open(records.head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!opened) return opened.status();
        const auto &head = opened.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.max_blocks != binding.layout.journal_blocks || head.max_blocks <= 7 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("allocated empty active sequence or fixed journal ring");
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(opened.value(), reader, binding.structural_metadata(),
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &plan = parsed.value();
        if (plan.empty_log || plan.incomplete_tail || plan.committed_transactions != 1 ||
            plan.revoked_actions || plan.revoke_records || plan.actions.size() != 4 || plan.next_sequence != head.sequence + 1)
            return Status::io_error("allocated empty complete committed journal grammar");
        for (usize home = 0; home < 4; ++home)
        {
            const auto &action = plan.actions[home];
            if (action.sequence != head.sequence || action.transaction_order || action.journal_block != home + 2 ||
                action.target_block != ext4_checked::le32(records.descriptor, 192 + home * 4))
                return Status::io_error("allocated empty exact journal action identity");
            scratch.after[home] = records.payloads[home];
            if (action.restore_magic) be32(scratch.after[home], 0, jbd2::magic);
        }
        return {};
    }
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataJournalRecords &records, const Block &inode_bitmap,
        std::span<const Block> actual_homes, Ext4AllocatedEmptyDataJournalScratch &scratch,
        Ext4AllocatedEmptyDataJournalResult &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(output, binding) ||
            overlap(scratch, records) || overlap(output, records) || overlap(scratch, inode_bitmap) ||
            overlap(output, inode_bitmap))
            return Status::invalid_argument("allocated empty journal request/workspace overlap");
        for (const auto *block : {&records.head, &records.descriptor, &records.commit, &records.end})
            if (overlap(scratch, *block) || overlap(output, *block))
                return Status::invalid_argument("allocated empty journal record/workspace overlap");
        for (const auto images : {records.payloads, actual_homes})
            if (overlaps(&scratch, sizeof(scratch), images.data(), images.size_bytes()) ||
                overlaps(&output, sizeof(output), images.data(), images.size_bytes()))
                return Status::invalid_argument("allocated empty journal image/workspace overlap");
        scratch = {}; output = {};
        if (actual_homes.size() != 4)
            return Status::invalid_argument("allocated empty actual whole-home count");
        if (auto status = parse(binding, records, scratch); !status) return status;
        Ext4AllocatedEmptyDataWitness witness{};
        if (auto status = Ext4AllocatedEmptyDataWitnessCodec::decode(binding, records.descriptor, scratch.after,
                inode_bitmap, scratch.witness, scratch.decoded, witness); !status) return status;
        if (witness.sequence != binding.journal_sequence)
            return Status::io_error("allocated empty witness active sequence differs");
        // A committed timestamp must describe this exact transform. Require the
        // private synchronous commit profile, including every unused byte.
        scratch.record = {};
        be32(scratch.record, 0, jbd2::magic); be32(scratch.record, 4, u32(jbd2::RecordType::commit));
        be32(scratch.record, 8, witness.sequence);
        be32(scratch.record, 52, witness.update.seconds); be32(scratch.record, 56, witness.update.nanoseconds);
        be32(scratch.record, 16, jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), scratch.record));
        if (scratch.record != records.commit)
            return Status::io_error("allocated empty exact commit image differs");
        u8 mask{};
        if (auto status = Ext4AllocatedEmptyDataHomeMatcher::classify(scratch.decoded,
                std::span<const Block, 4>{actual_homes.data(), 4}, mask); !status) return status;
        output.witness = witness; output.active_sequence = witness.sequence;
        output.next_sequence = witness.sequence + 1; output.after_mask = mask; output.complete = true;
        return {};
    }
};


struct Ext4ZeroTruncateJournalFormat
{
    Ext4ZeroTruncateBlock head{}, descriptor{}, commit{}, end{}, empty_head{};
    std::array<Ext4ZeroTruncateBlock, 4> payloads{};
    u32 active_sequence{}, next_sequence{};
    bool ready{};
};
struct Ext4ZeroTruncateJournalFormatScratch
{
    Ext4ZeroTruncateJournalScratch verification{};
    Ext4ZeroTruncateJournalResult result{};
    Ext4RecoveryInfo active{};
};

// Pure formatting and complete independent regeneration. Public images do not
// admit publication, replay, lifetime transitions, WRITE or FLUSH. A future
// canonical owner must reserve and retain its exact transport before dispatch.
class Ext4ZeroTruncateJournalFormatter final
{
    using Block = Ext4ZeroTruncateBlock;
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(type)); be32(bytes, 8, sequence); }
    [[nodiscard]] static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static bool same_plan(const Ext4ZeroTruncatePlan &a, const Ext4ZeroTruncatePlan &b)
    {
        return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size &&
            a.group == b.group && a.released_count == b.released_count && a.released_mapping == b.released_mapping &&
            a.targets == b.targets && a.before == b.before && a.after == b.after && a.count == b.count && a.noop == b.noop;
    }
  public:
    [[nodiscard]] static Status format(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncatePlan &plan, const Block &old_head, const Block &inode_bitmap,
        Ext4ZeroTruncateJournalFormatScratch &scratch, Ext4ZeroTruncateJournalFormat &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(scratch, plan) ||
            overlap(scratch, old_head) || overlap(scratch, inode_bitmap) || overlap(output, binding) ||
            overlap(output, plan) || overlap(output, old_head) || overlap(output, inode_bitmap))
            return Status::invalid_argument("zero truncate journal format workspace overlap");
        scratch = {}; output = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            !binding.structural_metadata().valid() || plan.count != 4 || plan.noop ||
            !plan.released_count || plan.released_count > 16)
            return Status::unsupported("zero truncate journal format bounded dirty empty profile");
        const auto codec = jbd2::Codec::open(old_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block || head.max_blocks <= 7 ||
            head.sequence != binding.journal_sequence || !head.sequence || head.sequence > 0xfffffffcU ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("zero truncate journal format fixed ring or sequence capacity");
        output.active_sequence = head.sequence + 1; output.next_sequence = output.active_sequence + 1;
        header(output.descriptor, jbd2::RecordType::descriptor, output.active_sequence);
        header(output.commit, jbd2::RecordType::commit, output.active_sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{output.descriptor}.subspan(8, 4);
        for (usize home = 0; home < 4; ++home)
        {
            output.payloads[home] = plan.after[home];
            u32 flags = (home ? jbd2::flag_same_uuid : 0) | (home == 3 ? jbd2::flag_last_tag : 0);
            if (jbd2::detail::be32(output.payloads[home], 0) == jbd2::magic)
            { be32(output.payloads[home], 0, 0); flags |= jbd2::flag_escape; }
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            be32(output.descriptor, at, plan.targets[home]); be32(output.descriptor, at + 4, flags);
            be32(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), output.payloads[home]));
        }
        if (auto status = Ext4ZeroTruncateWitnessCodec::encode(binding, plan, inode_bitmap, output.active_sequence,
                scratch.verification.witness, output.descriptor); !status) return reject(status);
        be32(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        // The codec derives the checked update from the canonical after inode.
        be32(output.commit, 52, ext4_checked::le32(output.descriptor, 180));
        be32(output.commit, 56, ext4_checked::le32(output.descriptor, 184));
        be32(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        output.head = old_head; be32(output.head, 24, output.active_sequence); be32(output.head, 28, 1);
        be32(output.head, 252, 0); be32(output.head, 252, jbd2::detail::crc32c(~u32{0}, output.head));
        output.empty_head = old_head; be32(output.empty_head, 24, output.next_sequence); be32(output.empty_head, 28, 0);
        be32(output.empty_head, 252, 0); be32(output.empty_head, 252, jbd2::detail::crc32c(~u32{0}, output.empty_head));
        scratch.active = binding; scratch.active.empty_log = false; scratch.active.journal_sequence = output.active_sequence;
        const Ext4ZeroTruncateJournalRecords records{output.head, output.descriptor, output.commit, output.end, output.payloads};
        if (auto status = Ext4ZeroTruncateJournalVerifier::verify(scratch.active, records, inode_bitmap,
                plan.before, scratch.verification, scratch.result); !status) return reject(status);
        if (!scratch.result.complete || scratch.result.after_mask || !same_plan(scratch.verification.decoded, plan) ||
            scratch.result.active_sequence != output.active_sequence || scratch.result.next_sequence != output.next_sequence)
            return reject(Status::fault("zero truncate generated journal roundtrip differs"));
        const auto empty = jbd2::Codec::open(output.empty_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!empty) return reject(empty.status());
        if (empty.value().superblock().start_block || empty.value().superblock().sequence != output.next_sequence)
            return reject(Status::fault("zero truncate generated empty tail differs"));
        output.ready = true; return {};
    }
};

struct Ext4AllocatedEmptyDataJournalFormat
{
    Ext4FirstDataBlock head{}, descriptor{}, commit{}, end{}, empty_head{};
    std::array<Ext4FirstDataBlock, 4> payloads{};
    u32 active_sequence{}, next_sequence{};
    bool ready{};
};
struct Ext4AllocatedEmptyDataJournalFormatScratch
{
    Ext4AllocatedEmptyDataJournalScratch verification{};
    Ext4AllocatedEmptyDataJournalResult result{};
    Ext4RecoveryInfo active{};
};

// Pure formatting and complete independent regeneration. Public images do not
// admit publication, replay, lifetime transitions, WRITE or FLUSH. A future
// canonical owner must reserve and retain its exact transport before dispatch.
class Ext4AllocatedEmptyDataJournalFormatter final
{
    using Block = Ext4FirstDataBlock;
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(type)); be32(bytes, 8, sequence); }
    [[nodiscard]] static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static bool same_plan(const Ext4AllocatedEmptyDataPlan &a, const Ext4AllocatedEmptyDataPlan &b)
    {
        return a.update.inode == b.update.inode && a.update.generation == b.update.generation &&
            a.update.size == b.update.size && a.update.seconds == b.update.seconds &&
            a.update.nanoseconds == b.update.nanoseconds && a.group == b.group && a.physical == b.physical &&
            a.old_version == b.old_version && a.new_version == b.new_version && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count;
    }
  public:
    [[nodiscard]] static Status format(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataPlan &plan, const Block &old_head, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataJournalFormatScratch &scratch, Ext4AllocatedEmptyDataJournalFormat &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(scratch, plan) ||
            overlap(scratch, old_head) || overlap(scratch, inode_bitmap) || overlap(output, binding) ||
            overlap(output, plan) || overlap(output, old_head) || overlap(output, inode_bitmap))
            return Status::invalid_argument("allocated empty journal format workspace overlap");
        scratch = {}; output = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            !binding.structural_metadata().valid() || plan.count != 4 || !plan.update.size || plan.update.size > 1024)
            return Status::unsupported("allocated empty journal format bounded dirty empty profile");
        const auto codec = jbd2::Codec::open(old_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block || head.max_blocks <= 7 ||
            head.sequence != binding.journal_sequence || !head.sequence || head.sequence > 0xfffffffcU ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("allocated empty journal format fixed ring or sequence capacity");
        output.active_sequence = head.sequence + 1; output.next_sequence = output.active_sequence + 1;
        header(output.descriptor, jbd2::RecordType::descriptor, output.active_sequence);
        header(output.commit, jbd2::RecordType::commit, output.active_sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{output.descriptor}.subspan(8, 4);
        for (usize home = 0; home < 4; ++home)
        {
            output.payloads[home] = plan.after[home];
            u32 flags = (home ? jbd2::flag_same_uuid : 0) | (home == 3 ? jbd2::flag_last_tag : 0);
            if (jbd2::detail::be32(output.payloads[home], 0) == jbd2::magic)
            { be32(output.payloads[home], 0, 0); flags |= jbd2::flag_escape; }
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            be32(output.descriptor, at, plan.targets[home]); be32(output.descriptor, at + 4, flags);
            be32(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), output.payloads[home]));
        }
        if (auto status = Ext4AllocatedEmptyDataWitnessCodec::encode(binding, plan, inode_bitmap, output.active_sequence,
                scratch.verification.witness, output.descriptor); !status) return reject(status);
        be32(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        // The witness carries the exact checked update regenerated from the old inode.
        be32(output.commit, 52, ext4_checked::le32(output.descriptor, 184));
        be32(output.commit, 56, ext4_checked::le32(output.descriptor, 188));
        be32(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        output.head = old_head; be32(output.head, 24, output.active_sequence); be32(output.head, 28, 1);
        be32(output.head, 252, 0); be32(output.head, 252, jbd2::detail::crc32c(~u32{0}, output.head));
        output.empty_head = old_head; be32(output.empty_head, 24, output.next_sequence); be32(output.empty_head, 28, 0);
        be32(output.empty_head, 252, 0); be32(output.empty_head, 252, jbd2::detail::crc32c(~u32{0}, output.empty_head));
        scratch.active = binding; scratch.active.empty_log = false; scratch.active.journal_sequence = output.active_sequence;
        const Ext4AllocatedEmptyDataJournalRecords records{output.head, output.descriptor, output.commit, output.end, output.payloads};
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::verify(scratch.active, records, inode_bitmap,
                plan.before, scratch.verification, scratch.result); !status) return reject(status);
        if (!scratch.result.complete || scratch.result.after_mask || !same_plan(scratch.verification.decoded, plan) ||
            scratch.result.active_sequence != output.active_sequence || scratch.result.next_sequence != output.next_sequence)
            return reject(Status::fault("allocated empty generated journal roundtrip differs"));
        const auto empty = jbd2::Codec::open(output.empty_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!empty) return reject(empty.status());
        if (empty.value().superblock().start_block || empty.value().superblock().sequence != output.next_sequence)
            return reject(Status::fault("allocated empty generated empty tail differs"));
        output.ready = true; return {};
    }
};

template <jbd2::BlockReader Reader> class Ext4AllocatedEmptyDataPreview;

// Permanent one-shot scratch claim. This storage and its supplied transport
// must outlive the preview and remain exclusive; allocate outside kernel stacks.
template <jbd2::BlockReader Reader>
class Ext4AllocatedEmptyDataPreviewWorkspace final
{
    friend class Ext4AllocatedEmptyDataPreview<Reader>;
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Block = Ext4FirstDataBlock;
    struct Overlay
    {
        Ext4AllocatedEmptyDataPreviewWorkspace *workspace;
        bool final;
        Status read_block(u32 number, std::span<u8> out)
        {
            if (!workspace->journal_ready_ || out.size() < 1024)
                return Status::not_initialized("allocated empty data ownership overlay is unavailable");
            for (usize home = 0; home < 4; ++home)
                if (number == workspace->journal_.decoded.targets[home])
                {
                    const auto &bytes = final ? workspace->journal_.decoded.after[home] : workspace->journal_.decoded.before[home];
                    for (usize byte = 0; byte < 1024; ++byte) out[byte] = bytes[byte];
                    return {};
                }
            return workspace->read(number, out.first(1024));
        }
    };
  public:
    Ext4AllocatedEmptyDataPreviewWorkspace() = default;
    Ext4AllocatedEmptyDataPreviewWorkspace(const Ext4AllocatedEmptyDataPreviewWorkspace &) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace &operator=(const Ext4AllocatedEmptyDataPreviewWorkspace &) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace(Ext4AllocatedEmptyDataPreviewWorkspace &&) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace &operator=(Ext4AllocatedEmptyDataPreviewWorkspace &&) = delete;
  private:
    Status read(u32 number, std::span<u8> out)
    {
        if (!claimant_ || !reader_ || out.size() != 1024 || number >= binding_.layout.filesystem_blocks)
            return Status::invalid_argument("allocated empty data preview transport read bounds");
        if (read_attempts_ == ~usize{0}) return Status::overflow("allocated empty data preview read counter exhausted");
        ++read_attempts_;
        if (auto status = reader_->read_block(number, out); !status) return status;
        ++completed_reads_; return {};
    }
    const void *claimant_{};
    Reader *reader_{};
    Ext4RecoveryInfo binding_{};
    usize read_attempts_{}, completed_reads_{};
    u32 inode_bitmap_target_{};
    bool journal_ready_{};
    Block head_{}, descriptor_{}, commit_{}, end_{}, inode_bitmap_{}, data_{}, checked_{};
    std::array<Block, 4> payloads_{}, homes_{};
    Ext4AllocatedEmptyDataJournalScratch journal_{};
    Ext4AllocatedEmptyDataJournalResult result_{};
    Ext4AllocatedEmptyDataOwnershipScratch old_ownership_{};
    Ext4ZeroTruncateOwnershipScratch final_ownership_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4AllocatedEmptyDataOwnership old_proof_{};
    Ext4ZeroTruncateOwnership final_proof_{};
    Overlay old_reader_{this, false}, final_reader_{this, true};
    Ext4ReadOnlyView<Overlay> old_view_{}, final_view_{};
};

enum class Ext4AllocatedEmptyDataPreviewPhase : u8 { unused, capturing, journal, ownership, readback, ready, failed };
struct Ext4AllocatedEmptyDataPreviewReport
{
    Ext4AllocatedEmptyDataPreviewPhase phase{Ext4AllocatedEmptyDataPreviewPhase::unused};
    Status status{};
    usize read_attempts{}, completed_reads{};
    u32 inode{}, generation{}, physical{}, group{}, size{}, active_sequence{}, next_sequence{};
    u64 old_version{}, new_version{};
    u8 after_mask{};
    bool old_ownership{}, final_ownership{}, snapshot_readback{};
};

// Read-only typed preview from an exclusive caller-protected transport. It
// captures the complete journal and homes, proves full old/final ownership via
// exact overlays, then rechecks captured bytes. No Session or replay permission
// is manufactured. A canonical mounted owner and counter reservation are later.
template <jbd2::BlockReader Reader>
class Ext4AllocatedEmptyDataPreview final
{
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Workspace = Ext4AllocatedEmptyDataPreviewWorkspace<Reader>;
    using Block = Ext4FirstDataBlock;
  public:
    Ext4AllocatedEmptyDataPreview() = default;
    Ext4AllocatedEmptyDataPreview(const Ext4AllocatedEmptyDataPreview &) = delete;
    Ext4AllocatedEmptyDataPreview &operator=(const Ext4AllocatedEmptyDataPreview &) = delete;
    Ext4AllocatedEmptyDataPreview(Ext4AllocatedEmptyDataPreview &&) = delete;
    Ext4AllocatedEmptyDataPreview &operator=(Ext4AllocatedEmptyDataPreview &&) = delete;
    [[nodiscard]] const Ext4AllocatedEmptyDataPreviewReport &report() const { return report_; }
    [[nodiscard]] Status prepare(Reader &reader, const Ext4RecoveryInfo &binding, Workspace &workspace)
    {
        if (report_.phase != Ext4AllocatedEmptyDataPreviewPhase::unused)
            return Status::already_exists("allocated empty data preview is one-shot");
        const auto overlap = [](const void *a, usize a_size, const void *b, usize b_size)
        {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < a_size : one - two < b_size;
        };
        // Refuse borrowing aliases before changing any potentially aliased byte.
        if (overlap(this, sizeof(*this), &workspace, sizeof(workspace)) ||
            overlap(this, sizeof(*this), &binding, sizeof(binding)) ||
            overlap(this, sizeof(*this), &reader, sizeof(reader)) ||
            overlap(&binding, sizeof(binding), &workspace, sizeof(workspace)) ||
            overlap(&reader, sizeof(reader), &workspace, sizeof(workspace)))
            return Status::invalid_argument("allocated empty data preview owner/workspace/input overlap");
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::capturing;
        if (workspace.claimant_) return fail(Status::already_exists("allocated empty data preview workspace already claimed"));
        workspace.claimant_ = this; workspace_ = &workspace;
        workspace.reader_ = &reader; workspace.binding_ = binding;
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks > binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return fail(Status::invalid_argument("allocated empty data preview bounded active layout"));
        const u32 first = u32(binding.layout.journal_first_block);
        for (usize index = 0; index < 8; ++index)
            if (auto status = workspace.read(first + u32(index), record(index)); !status) return fail(status);
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::journal;
        const Ext4AllocatedEmptyDataJournalRecords records{workspace.head_, workspace.descriptor_, workspace.commit_, workspace.end_, workspace.payloads_};
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::parse(workspace.binding_, records, workspace.journal_); !status)
            return fail(status);
        const u32 inode = ext4_checked::le32(workspace.descriptor_, 164);
        const u32 per_group = ext4_checked::le32(workspace.journal_.after[2], 40);
        if (!per_group || !inode || inode > ext4_checked::le32(workspace.journal_.after[2], 0) ||
            (inode - 1) / per_group >= binding.metadata_count / 4)
            return fail(Status::io_error("allocated empty data preview captured inode geometry"));
        const auto bitmap = binding.metadata[((inode - 1) / per_group) * 4 + 2];
        if (bitmap.count != 1 || bitmap.first > 0xffffffffULL)
            return fail(Status::io_error("allocated empty data preview inode bitmap binding"));
        workspace.inode_bitmap_target_ = u32(bitmap.first);
        if (auto status = workspace.read(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status)
            return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = workspace.read(u32(workspace.journal_.actions[home].target_block), workspace.homes_[home]); !status)
                return fail(status);
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::verify(workspace.binding_, records,
                workspace.inode_bitmap_, workspace.homes_, workspace.journal_, workspace.result_); !status) return fail(status);
        const auto &plan = workspace.journal_.decoded;
        if (auto status = workspace.read(plan.physical, workspace.data_); !status) return fail(status);
        workspace.journal_ready_ = true;
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::ownership;
        if (auto status = workspace.old_view_.open(workspace.old_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        if (auto status = Ext4AllocatedEmptyDataOwnershipValidator::prove(workspace.binding_, workspace.old_view_,
                plan, workspace.old_ownership_, workspace.old_proof_); !status) return fail(status);
        if (!workspace.old_proof_.complete || workspace.old_proof_.physical != plan.physical ||
            workspace.old_proof_.group != plan.group || workspace.old_proof_.old_version != plan.old_version)
            return fail(Status::io_error("allocated empty data old overlay candidate differs"));
        report_.old_ownership = true;
        if (auto status = workspace.final_view_.open(workspace.final_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        const Ext4ZeroTruncateUpdate filled{plan.update.inode, plan.update.generation, plan.update.size, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.final_view_,
                filled, workspace.final_ownership_, workspace.final_proof_); !status) return fail(status);
        if (workspace.final_proof_.count != 1 || workspace.final_proof_.mapping[0] != plan.physical ||
            workspace.final_proof_.group != plan.group || workspace.final_proof_.inode != plan.update.inode ||
            workspace.final_proof_.generation != plan.update.generation || workspace.final_proof_.old_size != plan.update.size)
            return fail(Status::io_error("allocated empty data final overlay mapping or identity differs"));
        report_.final_ownership = true; report_.phase = Ext4AllocatedEmptyDataPreviewPhase::readback;
        for (usize index = 0; index < 8; ++index)
            if (auto status = check(first + u32(index), record(index)); !status) return fail(status);
        if (auto status = check(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status) return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = check(plan.targets[home], workspace.homes_[home]); !status) return fail(status);
        if (auto status = check(plan.physical, workspace.data_); !status) return fail(status);
        report_.snapshot_readback = true;
        report_.inode = plan.update.inode; report_.generation = plan.update.generation;
        report_.physical = plan.physical; report_.group = plan.group; report_.size = plan.update.size;
        report_.old_version = plan.old_version; report_.new_version = plan.new_version;
        report_.active_sequence = workspace.result_.active_sequence; report_.next_sequence = workspace.result_.next_sequence;
        report_.after_mask = workspace.result_.after_mask;
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::ready; update_counts(); return {};
    }
  private:
    Block &record(usize index)
    {
        if (!index) return workspace_->head_;
        if (index == 1) return workspace_->descriptor_;
        if (index < 6) return workspace_->payloads_[index - 2];
        return index == 6 ? workspace_->commit_ : workspace_->end_;
    }
    [[nodiscard]] Status check(u32 number, const Block &expected)
    {
        if (auto status = workspace_->read(number, workspace_->checked_); !status) return status;
        return workspace_->checked_ == expected ? Status{} : Status::io_error("allocated empty data captured source changed during preview");
    }
    void update_counts()
    {
        if (workspace_)
        { report_.read_attempts = workspace_->read_attempts_; report_.completed_reads = workspace_->completed_reads_; }
    }
    [[nodiscard]] Status fail(Status status)
    { report_.phase = Ext4AllocatedEmptyDataPreviewPhase::failed; report_.status = status; update_counts(); return status; }
    Workspace *workspace_{};
    Ext4AllocatedEmptyDataPreviewReport report_{};
};

enum class Ext4ZeroTruncateRoute : u8 { fresh, recovery, noop };
struct Ext4ZeroTruncateTransactionCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 groups{}, final_sequence{};
    Ext4ZeroTruncateRoute route{};
    friend bool operator==(const Ext4ZeroTruncateTransactionCapacity &,
                           const Ext4ZeroTruncateTransactionCapacity &) = default;
};

// Pure reservation arithmetic. Its returned facts grant no I/O authority.
class Ext4ZeroTruncateTransactionBudget final
{
  public:
    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4ZeroTruncateTransactionCapacity> capacity(
        const block::SessionReport &initial, u32 groups, Ext4ZeroTruncateRoute route, u32 sequence)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !sequence ||
            (route != Ext4ZeroTruncateRoute::fresh && route != Ext4ZeroTruncateRoute::recovery &&
             route != Ext4ZeroTruncateRoute::noop))
            return Status::invalid_argument("zero truncate whole route or group capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("zero truncate whole route requires a stable original Session");
        Ext4ZeroTruncateTransactionCapacity result{};
        result.groups = groups; result.route = route; result.final_sequence = sequence;
        // A=1420+531G: one strict opening and full ownership proof.
        // B=2866+1062G: accepted active preview, including its captures.
        if (route == Ext4ZeroTruncateRoute::fresh)
        {
            if (sequence > 0xfffffffcU) return Status::overflow("zero truncate fresh sequence reservation exhausted");
            result.reads = 5849 + 2124 * block::SessionCounter(groups); // B+2A+13+10*13.
            result.writes = 13; result.flushes = 6; result.final_sequence += 2;
        }
        else if (route == Ext4ZeroTruncateRoute::recovery)
        {
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("zero truncate recovery sequence reservation exhausted");
            result.reads = 4351 + 1593 * block::SessionCounter(groups); // B+A+5*13.
            result.writes = 5; result.flushes = 3; ++result.final_sequence;
        }
        else result.reads = 1446 + 531 * block::SessionCounter(groups); // A+13+13.
        const auto fits = [](block::SessionCounter initial_count, block::SessionCounter count) {
            return block::session_counter_fits<Counter>(initial_count, count);
        };
        if (!fits(initial.reads, result.reads) || !fits(initial.completed_reads, result.reads) ||
            !fits(initial.writes, result.writes) || !fits(initial.completed_writes, result.writes) ||
            !fits(initial.flushes, result.flushes) || !fits(initial.completed_flushes, result.flushes))
            return Status::overflow("zero truncate whole attempted/completed counters exhausted");
        return result;
    }
};

enum class Ext4AllocatedEmptyDataRoute : u8 { fresh, recovery };
struct Ext4AllocatedEmptyDataTransactionCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 groups{}, final_sequence{};
    Ext4AllocatedEmptyDataRoute route{};
    friend bool operator==(const Ext4AllocatedEmptyDataTransactionCapacity &,
                           const Ext4AllocatedEmptyDataTransactionCapacity &) = default;
};

// Pure whole-route arithmetic; no returned fact grants transport authority.
class Ext4AllocatedEmptyDataTransactionBudget final
{
  public:
    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4AllocatedEmptyDataTransactionCapacity> capacity(
        const block::SessionReport &initial, u32 groups, Ext4AllocatedEmptyDataRoute route, u32 sequence)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !sequence ||
            (route != Ext4AllocatedEmptyDataRoute::fresh && route != Ext4AllocatedEmptyDataRoute::recovery))
            return Status::invalid_argument("allocated empty whole route or group capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("allocated empty whole route requires a stable original Session");
        Ext4AllocatedEmptyDataTransactionCapacity result{};
        result.groups = groups; result.route = route; result.final_sequence = sequence;
        // A=1420+531G: checked opening plus complete ownership.
        // B=2876+1062G: active allocated-empty preview.
        if (route == Ext4AllocatedEmptyDataRoute::fresh)
        {
            if (sequence > 0xfffffffcU)
                return Status::overflow("allocated empty fresh sequence reservation exhausted");
            result.reads = 5884 + 2125 * block::SessionCounter(groups); // B+2A+G+14+11*14.
            result.writes = 14; result.flushes = 7; result.final_sequence += 2;
        }
        else
        {
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("allocated empty recovery sequence reservation exhausted");
            result.reads = 4366 + 1593 * block::SessionCounter(groups); // B+A+5*14.
            result.writes = 5; result.flushes = 3; ++result.final_sequence;
        }
        const auto fits = [](block::SessionCounter used, block::SessionCounter count) {
            return block::session_counter_fits<Counter>(used, count);
        };
        if (!fits(initial.reads, result.reads) || !fits(initial.completed_reads, result.reads) ||
            !fits(initial.writes, result.writes) || !fits(initial.completed_writes, result.writes) ||
            !fits(initial.flushes, result.flushes) || !fits(initial.completed_flushes, result.flushes))
            return Status::overflow("allocated empty whole attempted/completed counters exhausted");
        return result;
    }
};
} // namespace ok::fs
