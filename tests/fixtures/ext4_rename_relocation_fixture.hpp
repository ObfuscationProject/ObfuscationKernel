#pragma once
#include <ok/fs/ext4_rename_plan.hpp>

namespace ok::testing::rename_fixture
{
using Inode = std::array<u8, 256>;
using Block = std::array<u8, 1024>;
inline constexpr usize tail = 1012, source_offset = 24;
inline void put16(std::span<u8> bytes, usize offset, u16 value)
{ bytes[offset] = u8(value); bytes[offset + 1] = u8(value >> 8); }
inline void put32(std::span<u8> bytes, usize offset, u32 value)
{ put16(bytes, offset, u16(value)); put16(bytes, offset + 2, u16(value >> 16)); }
inline void entry(Block &block, usize offset, u16 record, u32 inode, u8 type, std::string_view name)
{
    for (usize i = 0; i < record; ++i) block[offset + i] = 0;
    put32(block, offset, inode); put16(block, offset + 4, record);
    block[offset + 6] = u8(name.size()); block[offset + 7] = type;
    for (usize i = 0; i < name.size(); ++i) block[offset + 8 + i] = u8(name[i]);
}
struct Fixture
{
    fs::Ext4RecoveryInfo binding{};
    Inode directory{}, source{};
    Block image{};
    Fixture()
    {
        binding.uuid.fill(0x3d);
        binding.layout.block_size = 1024; binding.layout.filesystem_blocks = 256;
        binding.layout.journal_first_block = 80; binding.layout.journal_blocks = 32;
        binding.metadata_count = 1; binding.metadata[0] = {0, 2};
        inode(directory, 0040700, 2, 1024, 20);
        inode(source, 0100600, 1, 23, 40);
        seal_inode(directory, 2); seal_inode(source, 12);
        entry(image, 0, 12, 2, 2, "."); entry(image, 12, 12, 2, 2, "..");
        entry(image, 24, 20, 12, 1, "notes.txt"); entry(image, 44, 968, 0, 0, "");
        seal();
    }
    static void inode(Inode &out, u16 mode, u16 links, u32 size, u32 block)
    {
        put16(out, 0, mode); put16(out, 26, links); put32(out, 4, size);
        put32(out, 28, 2); put32(out, 32, 0x80000); put32(out, 100, 7);
        put16(out, 128, 32); put16(out, 40, 0xf30a); put16(out, 42, 1);
        put16(out, 44, 4); put16(out, 56, 1); put32(out, 60, block);
    }
    u32 seed() const { return fs::jbd2::detail::crc32c(~u32{0}, binding.uuid); }
    void seal_inode(Inode &out, u32 number) const
    {
        put16(out, 124, 0); put16(out, 130, 0);
        const auto checksum = fs::jbd2::detail::crc32c(fs::ext4_checked::inode_seed(seed(), number, out), out);
        put16(out, 124, u16(checksum)); put16(out, 130, u16(checksum >> 16));
    }
    void seal()
    {
        put32(image, tail, 0); put16(image, tail + 4, 12); image[tail + 6] = 0; image[tail + 7] = 0xde;
        put32(image, 1020, fs::jbd2::detail::crc32c(fs::ext4_checked::inode_seed(seed(), 2, directory),
                                               std::span<const u8>{image}.first(tail)));
    }
    Result<fs::Ext4DirectoryRenamePlan> plan(std::string_view destination = "saved-note.txt") const
    { return fs::Ext4DirectoryRenamePlanner::plan(binding, 2, directory, 12, source, 20, image, "notes.txt", destination); }
};
inline usize find(const Block &block, std::string_view name)
{
    for (usize offset = 0; offset < tail; offset += fs::ext4_checked::le16(block, offset + 4))
        if (fs::ext4_checked::le32(block, offset) &&
            std::string_view{reinterpret_cast<const char *>(block.data() + offset + 8), block[offset + 6]} == name)
            return offset;
    return tail;
}
} // namespace ok::testing::rename_fixture
