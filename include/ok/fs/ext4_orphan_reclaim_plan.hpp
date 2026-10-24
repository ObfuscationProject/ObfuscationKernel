#pragma once

#include "ok/fs/ext4_orphan_ownership.hpp"

namespace ok::fs
{
struct Ext4OrphanReclaimPlan
{
    static constexpr usize capacity = 5;
    u32 inode{}, old_generation{}, retained_generation{}, next_head{};
    u32 released_blocks{};
    std::array<u32, 16> released_mapping{};
    std::array<u32, capacity> targets{};
    std::array<Ext4OrphanBlock, capacity> before{}, after{};
    usize count{};
};

// This is a pure old-image transformer. The caller must separately prove a
// stable, owner-exclusive device epoch and all runtime descriptor/pin lifetimes.
// A plan is neither journal authority nor permission to reclaim an inode.
class Ext4OrphanReclaimPlanner final
{
    using Block = Ext4OrphanBlock;

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
    }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index)
            bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group,
                                                  std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = descriptor[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static Status append(Ext4OrphanReclaimPlan &plan, u32 target,
                                       const Block &before, usize &index)
    {
        for (index = 0; index < plan.count; ++index)
            if (plan.targets[index] == target)
                return plan.before[index] == before ? Status{} :
                    Status::io_error("orphan reclaim aliased home images differ");
        if (plan.count == plan.targets.size())
            return Status::no_memory("orphan reclaim home capacity");
        index = plan.count++;
        plan.targets[index] = target;
        plan.before[index] = before;
        plan.after[index] = before;
        return {};
    }

  public:
    // Legacy pure transformer API retained for callers with an adequate stack.
    // Writable cold recovery uses the explicit caller-owned form below.
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4OrphanOwnershipSource &source, Ext4OrphanOwnershipScratch &scratch,
        u32 expected_head, u32 expected_generation, Ext4OrphanReclaimPlan &output)
    {
        Ext4OrphanReclaimPlan proposed{};
        return transform(binding, source, scratch, expected_head, expected_generation, proposed, output);
    }
    [[nodiscard]] static Status plan_off_stack(const Ext4RecoveryInfo &binding,
        const Ext4OrphanOwnershipSource &source, Ext4OrphanOwnershipScratch &scratch,
        u32 expected_head, u32 expected_generation, Ext4OrphanReclaimPlan &proposed,
        Ext4OrphanReclaimPlan &output)
    {
        const auto a = reinterpret_cast<usize>(&proposed), b = reinterpret_cast<usize>(&output);
        if (a <= b ? b - a < sizeof(proposed) : a - b < sizeof(output))
            return Status::invalid_argument("orphan reclaim scratch/output overlap");
        return transform(binding, source, scratch, expected_head, expected_generation, proposed, output);
    }
  private:
    [[nodiscard]] static Status transform(const Ext4RecoveryInfo &binding,
                                     const Ext4OrphanOwnershipSource &source,
                                     Ext4OrphanOwnershipScratch &scratch,
                                     u32 expected_head, u32 expected_generation,
                                     Ext4OrphanReclaimPlan &proposed, Ext4OrphanReclaimPlan &output)
    {
        using namespace ext4_checked;
        output = {}; proposed = {};
        if (!expected_head || expected_head < 11 || expected_head > 8192)
            return Status::invalid_argument("orphan reclaim expected head");
        Ext4OrphanOwnershipProof ownership{};
        if (auto status = Ext4OrphanOwnershipValidator::validate(
                binding, source, scratch, ownership); !status)
            return status;
        Ext4OrphanChainProof chain{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source.chain, chain); !status)
            return status;
        const auto *head = chain.at(0);
        if (!ownership.valid || ownership.filesystem_uuid != binding.uuid ||
            ownership.observed_head != expected_head ||
            chain.observed_head() != expected_head || !head ||
            head->number != expected_head || head->generation != expected_generation)
            return Status::io_error("orphan reclaim stale head or generation");
        if (head->generation == ~u32{0})
            return Status::overflow("orphan reclaim generation exhausted");
        if (head->data_count > head->data_blocks.size() ||
            head->data_count != (u64{head->size} + 1023) / 1024)
            return Status::unsupported("orphan reclaim bounded data mapping");

        const u32 inode_group = (head->number - 1) / 2048;
        const u32 inode_local = (head->number - 1) % 2048;
        const u32 data_group = head->data_count ? (head->data_blocks[0] - 1) / 8192 : 0;
        for (u32 index = 0; index < head->data_count; ++index)
            if ((head->data_blocks[index] - 1) / 8192 != data_group)
                return Status::unsupported("orphan reclaim spans data allocation groups");

        const auto &descriptors = source.chain.descriptors;
        const auto &superblock = source.chain.superblock;
        const u32 inode_table = le32(descriptors, inode_group * 32 + 8) + inode_local / 4;
        const u32 inode_bitmap = le32(descriptors, inode_group * 32 + 4);
        const u32 block_bitmap = head->data_count ? le32(descriptors, data_group * 32) : 0;
        const auto &table_before = source.inode_tables[inode_group][inode_local / 4];
        const u16 free_inodes = le16(descriptors, inode_group * 32 + 14);
        const u16 free_blocks = head->data_count ? le16(descriptors, data_group * 32 + 12) : 0;
        const u32 super_free_inodes = le32(superblock, 16);
        const u32 super_free_blocks = le32(superblock, 12);
        if (free_inodes >= 2048 || super_free_inodes >= 8192 ||
            (head->data_count &&
             (u32{free_blocks} + head->data_count > (data_group == 3 ? 8191U : 8192U) ||
              super_free_blocks > 32767U - head->data_count)))
            return Status::io_error("orphan reclaim free counters overflow");

        usize table_index{}, inode_bitmap_index{}, block_bitmap_index{};
        usize descriptor_index{}, super_index{};
        if (auto status = append(proposed, inode_table, table_before, table_index); !status)
            return status;
        if (auto status = append(proposed, inode_bitmap,
                                 source.chain.inode_bitmaps[inode_group],
                                 inode_bitmap_index); !status)
            return status;
        if (head->data_count)
            if (auto status = append(proposed, block_bitmap,
                                     source.chain.block_bitmaps[data_group],
                                     block_bitmap_index); !status)
                return status;
        if (auto status = append(proposed, 2, descriptors, descriptor_index); !status)
            return status;
        if (auto status = append(proposed, 1, superblock, super_index); !status)
            return status;
        if (proposed.count != (head->data_count ? 5U : 4U))
            return Status::unsupported("orphan reclaim prohibited home alias");

        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        auto inode = std::span<u8>{proposed.after[table_index]}.subspan(
            (inode_local % 4) * 256, 256);
        for (u8 &byte : inode) byte = 0;
        store32(inode, 100, head->generation + 1);
        store16(inode, 128, 32);
        // The exact private marker retains the next allocated identity. Create
        // must validate it, consume that generation and clear the entire slot.
        store32(inode, 160, 0x3146524fU); // "ORF1" in little-endian bytes.
        stamp_inode(inode, head->number, seed);

        auto &inode_bitmap_after = proposed.after[inode_bitmap_index];
        inode_bitmap_after[inode_local / 8] &= u8(~(1U << (inode_local % 8)));
        auto descriptor_after = std::span<u8>{proposed.after[descriptor_index]};
        store16(descriptor_after, inode_group * 32 + 14, u16(free_inodes + 1));
        store16(descriptor_after, inode_group * 32 + 26,
                u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmap_after}.first(256))));

        if (head->data_count)
        {
            auto &block_bitmap_after = proposed.after[block_bitmap_index];
            for (u32 index = 0; index < head->data_count; ++index)
            {
                const u32 local = (head->data_blocks[index] - 1) % 8192;
                block_bitmap_after[local / 8] &= u8(~(1U << (local % 8)));
            }
            store16(descriptor_after, data_group * 32 + 12,
                    u16(free_blocks + head->data_count));
            store16(descriptor_after, data_group * 32 + 24,
                    u16(jbd2::detail::crc32c(seed, std::span<const u8>{block_bitmap_after})));
        }
        // A single descriptor block may hold both changed groups. Stamp each
        // affected descriptor once after all counters and bitmaps are final.
        store16(descriptor_after, inode_group * 32 + 30, 0);
        store16(descriptor_after, inode_group * 32 + 30,
                descriptor_checksum(seed, inode_group,
                                    descriptor_after.subspan(inode_group * 32, 32)));
        if (head->data_count && data_group != inode_group)
        {
            store16(descriptor_after, data_group * 32 + 30, 0);
            store16(descriptor_after, data_group * 32 + 30,
                    descriptor_checksum(seed, data_group,
                                        descriptor_after.subspan(data_group * 32, 32)));
        }

        auto &super_after = proposed.after[super_index];
        store32(super_after, 16, super_free_inodes + 1);
        if (head->data_count)
            store32(super_after, 12, super_free_blocks + head->data_count);
        store32(super_after, 232, head->next);
        store32(super_after, 1020,
                jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        proposed.inode = head->number;
        proposed.old_generation = head->generation;
        proposed.retained_generation = head->generation + 1;
        proposed.next_head = head->next;
        proposed.released_blocks = head->data_count;
        for (u32 index = 0; index < head->data_count; ++index)
            proposed.released_mapping[index] = head->data_blocks[index];
        output = proposed;
        return {};
    }
};
} // namespace ok::fs
