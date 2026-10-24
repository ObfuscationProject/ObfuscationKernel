#pragma once

#include "ok/fs/ext4_next_data_plan.hpp"

namespace ok::fs
{
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
#include "ok/fs/ext4_completed_ownership_birth.inc"
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
} // namespace ok::fs
