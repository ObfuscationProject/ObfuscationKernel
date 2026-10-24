#pragma once

#include "ok/fs/ext4_readonly.hpp"

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
} // namespace ok::fs
