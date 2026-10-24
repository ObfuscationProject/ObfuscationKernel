#pragma once

#include "ok/fs/ext4_zero_truncate_plan.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
