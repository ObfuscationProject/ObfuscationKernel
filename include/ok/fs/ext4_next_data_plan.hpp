#pragma once

#include "ok/fs/ext4_first_data_plan.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
