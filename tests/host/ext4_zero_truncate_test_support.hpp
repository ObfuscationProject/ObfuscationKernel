#pragma once
#include <ok/fs/ext4_zero_truncate_plan.hpp>
using namespace ok;
using namespace ok::fs;
namespace zero_fixture
{
using Block = Ext4ZeroTruncateBlock;

void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
{
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 value = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
    put16(inode, 124, u16(value)); put16(inode, 130, u16(value >> 16));
}
void stamp_descriptor(std::span<u8> descriptor, u32 group, u32 seed)
{
    put16(descriptor, 30, 0);
    put16(descriptor, 30, u16(jbd2::detail::crc32c(
        jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), descriptor)));
}

struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block superblock{}, descriptors{}, bitmap{}, other_bitmap{}, inode_bitmap{}, inode_table{};
    u32 group{}, physical{}, inode{13}, table_block{8}, seed{};
    bool cross{};

    explicit Fixture(bool cross_group = false) : cross(cross_group)
    {
        group = cross ? 1 : 0;
        physical = cross ? 8261 : 70;
        binding.layout.block_size = 1024;
        binding.layout.filesystem_blocks = cross ? 8705 : 8193;
        binding.layout.journal_first_block = 1000;
        binding.layout.journal_blocks = 1024;
        binding.metadata_count = cross ? 8 : 4;
        binding.metadata[0] = {1, 2};
        binding.metadata[1] = {3, 1};
        binding.metadata[2] = {4, 1};
        binding.metadata[3] = {5, 64};
        if (cross)
        {
            binding.metadata[4] = {8193, 2};
            binding.metadata[5] = {8195, 1};
            binding.metadata[6] = {8196, 1};
            binding.metadata[7] = {8197, 64};
        }
        binding.journal_inode = 8;
        binding.needs_recovery = true;
        binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 1);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);

        bitmap.fill(0xff);
        other_bitmap.fill(0xff);
        const u32 local = (physical - 1) % 8192;
        bitmap[local / 8] &= u8(~(1U << (local % 8)));

        put32(superblock, 0, cross ? 512 : 256);
        put32(superblock, 4, u32(binding.layout.filesystem_blocks));
        put32(superblock, 12, 1); put32(superblock, 16, cross ? 512 : 256);
        put32(superblock, 20, 1);
        put32(superblock, 32, 8192); put32(superblock, 36, 8192);
        put32(superblock, 40, 256);
        put16(superblock, 56, 0xef53); put16(superblock, 58, 1);
        put32(superblock, 76, 1); put32(superblock, 84, 11);
        put16(superblock, 88, 256); put32(superblock, 92, 4);
        put32(superblock, 96, 0x46); put32(superblock, 100, 0x400);
        for (usize index = 0; index < binding.uuid.size(); ++index)
            superblock[104 + index] = binding.uuid[index];
        put32(superblock, 224, 8); put16(superblock, 254, 32);
        superblock[373] = 1;
        refresh_super();

        put32(descriptors, 0, 3); put32(descriptors, 4, 4); put32(descriptors, 8, 5);
        put16(descriptors, 12, cross ? 0 : 1);
        put16(descriptors, 14, 256); put16(descriptors, 18, 4);
        if (cross)
        {
            put32(descriptors, 32, 8195); put32(descriptors, 36, 8196);
            put32(descriptors, 40, 8197); put16(descriptors, 44, 1);
            put16(descriptors, 46, 256); put16(descriptors, 50, 4);
        }
        refresh_descriptor(0);
        if (cross) refresh_descriptor(1);

        auto node = std::span<u8>{inode_table}.first(256);
        put16(node, 0, 0100640);
        put16(node, 2, 1000); put16(node, 120, 1);
        put16(node, 24, 1001); put16(node, 122, 2);
        put16(node, 26, 1);
        for (const usize at : {usize{8}, usize{12}, usize{16}, usize{144}})
            put32(node, at, 1700000000);
        for (const usize at : {usize{140}, usize{132}, usize{136}, usize{148}})
            put32(node, at, 123456789U << 2);
        put32(node, 32, 0x80000);
        put16(node, 40, 0xf30a); put16(node, 44, 4);
        put16(node, 128, 32);
        put32(node, 100, 9); put32(node, 36, 0xfffffffeU); put32(node, 152, 7);
        inode_bitmap.fill(0); inode_bitmap[(inode - 1) / 8] |= u8(1U << ((inode - 1) % 8));
        put16(descriptors, 14, 255); put32(superblock, 16, cross ? 511 : 255);
        put16(descriptors, 26, u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmap}.first(32))));
        refresh_descriptor_crc(0); refresh_super();
        stamp_inode(node, inode, seed);
    }

    void refresh_super()
    { put32(superblock, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{superblock}.first(1020))); }
    void refresh_descriptor_crc(u32 number)
    { stamp_descriptor(std::span<u8>{descriptors}.subspan(number * 32, 32), number, seed); }
    void refresh_descriptor(u32 number)
    {
        const auto &bits = number == group ? bitmap : other_bitmap;
        const auto bytes = std::span<const u8>{bits}.first(1024);
        put16(descriptors, number * 32 + 24, u16(jbd2::detail::crc32c(seed, bytes)));
        refresh_descriptor_crc(number);
    }
    void refresh_inode()
    { stamp_inode(std::span<u8>{inode_table}.first(256), inode, seed); }
    [[nodiscard]] Ext4ZeroTruncateSource source() const
    { return {group, table_block, superblock, descriptors, bitmap, inode_bitmap, inode_table}; }
    [[nodiscard]] Ext4ZeroTruncateUpdate update() const
    { return {inode, 9, ext4_checked::le32(inode_table, 4), 1700000111, 765432100}; }
};

} // namespace zero_fixture
