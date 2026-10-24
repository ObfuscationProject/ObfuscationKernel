#pragma once
#include <ok/fs/ext4_create_plan.hpp>
#include <ok/fs/ext4_create_witness.hpp>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "create plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace mkdir_plan_fixture
{
using Block = Ext4CreateBlock;

void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
void inode_crc(std::span<u8> inode, u32 number, u32 seed)
{
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 value = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
    put16(inode, 124, u16(value)); put16(inode, 130, u16(value >> 16));
}

struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block superblock{}, descriptors{}, bitmap{}, parent_table{}, new_table{}, directory{};
    u32 parent{}, inode{}, parent_block{}, new_block{}, directory_block{100}, seed{};

    explicit Fixture(bool same_table = false)
    {
        parent = same_table ? 11 : 2;
        inode = same_table ? 12 : 13;
        parent_block = same_table ? 7 : 5;
        new_block = same_table ? 7 : 8;
        binding.layout.block_size = 1024;
        binding.layout.filesystem_blocks = 8193;
        binding.layout.journal_first_block = 1000;
        binding.layout.journal_blocks = 1024;
        binding.metadata_count = 4;
        binding.metadata[0] = {1, 2};
        binding.metadata[1] = {3, 1};
        binding.metadata[2] = {4, 1};
        binding.metadata[3] = {5, 64};
        binding.journal_inode = 8;
        binding.needs_recovery = true;
        binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 1);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        for (u32 number = 1; number < inode; ++number)
            bitmap[(number - 1) / 8] |= u8(1U << ((number - 1) % 8));

        put32(superblock, 0, 256); put32(superblock, 4, 8193);
        put32(superblock, 12, 1000); put32(superblock, 16, 257 - inode);
        put32(superblock, 20, 1); put32(superblock, 32, 8192);
        put32(superblock, 36, 8192); put32(superblock, 40, 256);
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
        put16(descriptors, 12, 1000); put16(descriptors, 14, u16(257 - inode));
        put16(descriptors, 16, 1); put16(descriptors, 18, 4);
        put16(descriptors, 28, u16(257 - inode));
        refresh_descriptor();

        auto node = std::span<u8>{parent_table}.subspan(((parent - 1) % 4) * 256, 256);
        put16(node, 0, 0040755); put32(node, 4, 1024); put16(node, 26, 2);
        put32(node, 28, 2); put32(node, 32, 0x80000);
        put16(node, 40, 0xf30a); put16(node, 42, 1); put16(node, 44, 4);
        put16(node, 56, 1); put32(node, 60, directory_block);
        put16(node, 128, 32); inode_crc(node, parent, seed);
        if (same_table) new_table = parent_table;

        put32(directory, 0, parent); put16(directory, 4, 12);
        directory[6] = 1; directory[7] = 2; directory[8] = '.';
        put32(directory, 12, parent); put16(directory, 16, 12);
        directory[18] = 2; directory[19] = 2; directory[20] = '.'; directory[21] = '.';
        put32(directory, 24, 3); put16(directory, 28, 988);
        directory[30] = 4; directory[31] = 1;
        directory[32] = 's'; directory[33] = 'e'; directory[34] = 'e'; directory[35] = 'd';
        put16(directory, 1016, 12); directory[1019] = 0xde;
        refresh_directory();
    }

    void refresh_super()
    { put32(superblock, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{superblock}.first(1020))); }
    void refresh_descriptor()
    {
        put16(descriptors, 26, u16(jbd2::detail::crc32c(seed, std::span<const u8>{bitmap}.first(32))));
        put16(descriptors, 30, 0);
        const u16 checksum = u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
            std::span<const u8>{descriptors}.first(32)));
        put16(descriptors, 30, checksum);
    }
    void refresh_directory()
    {
        const auto node = std::span<const u8>{parent_table}.subspan(((parent - 1) % 4) * 256, 256);
        put32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, parent, node), std::span<const u8>{directory}.first(1012)));
    }
    [[nodiscard]] Ext4CreateSource source() const
    { return {inode, 0, parent_block, new_block, directory_block,
              superblock, descriptors, bitmap, parent_table, new_table, directory}; }
    [[nodiscard]] Ext4CreateUpdate update(std::string_view name = "note") const
    { return {parent, 1700000111, 123456789, name, {NodeType::regular, 0640, 1000, 1001}}; }
};

} // namespace mkdir_plan_fixture
