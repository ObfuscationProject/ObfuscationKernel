#pragma once

#include "ok/fs/ext4_create_plan.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"

namespace ok::fs
{
// One exclusive old view supplies inode allocation, parent namespace, and the
// first supported free block. The owning transaction proves no allocated
// inode references physical before it can publish any image.
struct Ext4DirectoryCreateSource
{
    const Ext4CreateSource &inode;
    u32 block_group{}, physical{};
    const Ext4CreateBlock &block_bitmap;
    const Ext4CreateBlock &data;
};

// Pure planning for a single-block ordinary directory. Reuse the empty-create
// inode/namespace checks, then add checked block allocation, dot entries, link
// counts and used-directory accounting. No journal or device authority lives
// here. Nonzero old free-block bytes are refused by this bounded successor.
class Ext4DirectoryCreatePlanner final
{
    using Block = Ext4CreateBlock;
    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (8 * i)); }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    static void stamp_descriptor(std::span<u8> descriptor, u32 group, u32 seed)
    {
        store16(descriptor, 30, 0);
        store16(descriptor, 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), descriptor)));
    }

  public:
    // Read-only candidate admission never constructs a directory witness or
    // claims a zero preimage. Its images are the exact old participants; the
    // owner may stage them to reserve cache identity before initialization.
    [[nodiscard]] static Status admit_candidate(const Ext4RecoveryInfo &binding,
        const Ext4CreateUpdate &update, const Ext4DirectoryCreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::directory || (update.attributes.mode & ~u32{07777}))
            return Status::unsupported("directory candidate type or permission profile");
        auto regular = update;
        regular.attributes.type = NodeType::regular; regular.attributes.mode &= 0777;
        Ext4CreatePlan admitted{};
        if (auto status = Ext4CreatePlanner::plan(binding, regular, source.inode, admitted); !status)
            return status;
        const auto selected = Ext4FirstDataPlanner::select_block(binding, source.inode.superblock,
            source.inode.descriptors, source.block_bitmap, source.block_group);
        if (!selected) return selected.status();
        if (selected.value() != source.physical)
            return Status::invalid_argument("directory candidate is not first supported free block");
        const u32 per_group = le32(source.inode.superblock, 40);
        const usize parent_offset = ((update.parent - 1) % per_group % 4) * 256;
        if (le16(source.inode.parent_table, parent_offset + 26) == 0xffff ||
            le16(source.inode.descriptors, source.inode.group * 32 + 16) == 0xffff)
            return Status::overflow("directory candidate link or used-directory count exhausted");
        const u32 bitmap = le32(source.inode.descriptors, source.block_group * 32);
        for (usize index = 0; index < admitted.count; ++index)
            if (admitted.targets[index] == bitmap || admitted.targets[index] == source.physical)
                return Status::unsupported("directory candidate aliases an existing participant");
        if (bitmap == source.physical || admitted.count + 2 > admitted.targets.size())
            return Status::unsupported("directory candidate participant capacity or alias");
        const usize namespace_index = admitted.count - 1;
        const u32 namespace_block = admitted.targets[namespace_index];
        const Block namespace_before = admitted.before[namespace_index];
        admitted.targets[namespace_index] = bitmap;
        admitted.before[namespace_index] = source.block_bitmap;
        admitted.targets[admitted.count] = source.physical;
        admitted.before[admitted.count] = source.data;
        admitted.targets[admitted.count + 1] = namespace_block;
        admitted.before[admitted.count + 1] = namespace_before;
        admitted.count += 2;
        admitted.after = admitted.before;
        admitted.allocated_directory_block = source.physical;
        admitted.block_group = source.block_group;
        output = admitted;
        return {};
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4CreateUpdate &update,
                                     const Ext4DirectoryCreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::directory)
            return Status::unsupported("bounded mkdir requires a directory");
        if (update.attributes.mode & ~u32{07777})
            return Status::invalid_argument("bounded mkdir permission bits");
        auto regular = update;
        regular.attributes.type = NodeType::regular;
        regular.attributes.mode &= 0777;
        Ext4CreatePlan prepared{};
        if (auto status = Ext4CreatePlanner::plan(binding, regular, source.inode, prepared); !status)
            return status;
        const auto block = Ext4FirstDataPlanner::select_block(binding, source.inode.superblock,
            source.inode.descriptors, source.block_bitmap, source.block_group);
        if (!block) return block.status();
        if (block.value() != source.physical)
            return Status::invalid_argument("bounded mkdir block is not first supported free block");
        if (!jbd2::detail::all_zero(source.data))
            return Status::unsupported("bounded mkdir requires zeroed free block preimage");
        const u32 per_group = le32(source.inode.superblock, 40);
        const u32 blocks_per_group = le32(source.inode.superblock, 32);
        const usize parent_offset = ((update.parent - 1) % per_group % 4) * 256;
        const usize new_offset = ((source.inode.inode - 1) % per_group % 4) * 256;
        const auto old_parent = std::span<const u8>{source.inode.parent_table}.subspan(parent_offset, 256);
        if (le16(old_parent, 26) == 0xffff)
            return Status::overflow("bounded mkdir parent link count exhausted");
        const auto old_inode_descriptor = std::span<const u8>{source.inode.descriptors}.subspan(source.inode.group * 32, 32);
        if (le16(old_inode_descriptor, 16) == 0xffff)
            return Status::overflow("bounded mkdir used-directory count exhausted");
        const auto old_block_descriptor = std::span<const u8>{source.inode.descriptors}.subspan(source.block_group * 32, 32);
        const u32 bitmap_block = le32(old_block_descriptor, 0);
        for (usize i = 0; i < prepared.count; ++i)
            if (prepared.targets[i] == bitmap_block || prepared.targets[i] == source.physical)
                return Status::unsupported("bounded mkdir new participants alias inode or namespace images");
        if (bitmap_block == source.physical || prepared.count + 2 > prepared.targets.size())
            return Status::unsupported("bounded mkdir participant capacity or alias");
        const usize directory_index = prepared.count - 1;
        const Block parent_directory_before = prepared.before[directory_index];
        const Block parent_directory_after = prepared.after[directory_index];
        const u32 parent_directory_block = prepared.targets[directory_index];
        prepared.targets[directory_index] = bitmap_block;
        prepared.before[directory_index] = source.block_bitmap;
        prepared.after[directory_index] = source.block_bitmap;
        const usize data_index = prepared.count;
        prepared.targets[data_index] = source.physical;
        prepared.before[data_index] = source.data;
        prepared.after[data_index] = {};
        prepared.targets[data_index + 1] = parent_directory_block;
        prepared.before[data_index + 1] = parent_directory_before;
        prepared.after[data_index + 1] = parent_directory_after;
        prepared.count += 2;
        const usize parent_index = 0;
        const usize new_index = source.inode.parent_table_block == source.inode.new_table_block ? 0 : 1;
        const usize descriptor_index = new_index + 2;
        const usize super_index = new_index + 3;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        auto parent_after = std::span<u8>{prepared.after[parent_index]}.subspan(parent_offset, 256);
        store16(parent_after, 26, u16(le16(old_parent, 26) + 1));
        stamp_inode(parent_after, update.parent, seed);
        auto new_after = std::span<u8>{prepared.after[new_index]}.subspan(new_offset, 256);
        store16(new_after, 0, u16(0040000 | update.attributes.mode));
        store16(new_after, 26, 2);
        store32(new_after, 4, 1024); store32(new_after, 28, 2);
        store16(new_after, 42, 1); store16(new_after, 56, 1);
        store32(new_after, 60, source.physical);
        stamp_inode(new_after, source.inode.inode, seed);
        auto &bitmap_after = prepared.after[directory_index];
        const u32 local = source.physical - (1 + source.block_group * blocks_per_group);
        bitmap_after[local / 8] |= u8(1U << (local % 8));
        auto block_descriptor = std::span<u8>{prepared.after[descriptor_index]}.subspan(source.block_group * 32, 32);
        store16(block_descriptor, 12, u16(le16(old_block_descriptor, 12) - 1));
        store16(block_descriptor, 24, u16(jbd2::detail::crc32c(seed,
            std::span<const u8>{bitmap_after}.first(blocks_per_group / 8))));
        auto inode_descriptor = std::span<u8>{prepared.after[descriptor_index]}.subspan(source.inode.group * 32, 32);
        store16(inode_descriptor, 16, u16(le16(old_inode_descriptor, 16) + 1));
        stamp_descriptor(block_descriptor, source.block_group, seed);
        if (source.block_group != source.inode.group)
            stamp_descriptor(inode_descriptor, source.inode.group, seed);
        auto &super_after = prepared.after[super_index];
        store32(super_after, 12, le32(source.inode.superblock, 12) - 1);
        store32(super_after, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        auto &data_after = prepared.after[data_index];
        store32(data_after, 0, source.inode.inode); store16(data_after, 4, 12);
        data_after[6] = 1; data_after[7] = 2; data_after[8] = '.';
        store32(data_after, 12, update.parent); store16(data_after, 16, 1000);
        data_after[18] = 2; data_after[19] = 2; data_after[20] = data_after[21] = '.';
        store16(data_after, 1016, 12); data_after[1019] = 0xde;
        store32(data_after, 1020, jbd2::detail::crc32c(
            inode_seed(seed, source.inode.inode, new_after), std::span<const u8>{data_after}.first(1012)));
        auto &parent_directory = prepared.after[prepared.count - 1];
        bool inserted = false;
        for (usize offset = 0; offset < 1012; offset += le16(parent_directory, offset + 4))
            if (le32(parent_directory, offset) == source.inode.inode)
            { parent_directory[offset + 7] = 2; inserted = true; }
        if (!inserted) return Status::fault("bounded mkdir canonical insertion absent");
        store32(parent_directory, 1020, jbd2::detail::crc32c(
            inode_seed(seed, update.parent, parent_after), std::span<const u8>{parent_directory}.first(1012)));
        prepared.allocated_directory_block = source.physical;
        prepared.block_group = source.block_group;
        output = prepared;
        return {};
    }
};
} // namespace ok::fs
