// Extracted namespace model fixture; no production code or device writes.
#pragma once

#include "ok/fs/ext4/dir.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "namespace planner test:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace namespace_journal_model
{
using Block = Ext4NamespaceBlock;
using Image = std::vector<u8>;

class MemoryDisk final : public driver::BlockDevice
{
  public:
    explicit MemoryDisk(const Image &bytes) : bytes_(bytes) {}
    driver::BlockGeometry geometry() const override { return {bytes_.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > bytes_.size() / 512 || out.size() % 512 ||
            out.size() / 512 > bytes_.size() / 512 - sector)
            return Status::invalid_argument("namespace test read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(bytes_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("pure planner must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("pure planner must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &bytes_;
};

Block load(const Image &image, u32 number)
{
    CHECK(number < image.size() / 1024);
    Block result{};
    for (usize index = 0; index < result.size(); ++index)
        result[index] = image[number * 1024 + index];
    return result;
}
u32 value(const Block &image, usize at) { return ext4_checked::le32(image, at); }
u16 short_value(const Block &image, usize at) { return ext4_checked::le16(image, at); }
void store16(Block &image, usize at, u16 value)
{ image[at] = u8(value); image[at + 1] = u8(value >> 8); }
void store32(Block &image, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) image[at + index] = u8(value >> (index * 8)); }
usize slot(u32 inode) { return ((inode - 1) % 4) * 256; }

struct Fixture
{
    const Image &disk;
    const Ext4RecoveryInfo &binding;
    u32 parent_number{2}, source_number{14}, destination_number{15};
    u32 parent_block{}, source_block{}, destination_block{}, directory_block{};
    Block super{}, descriptors{}, parent{}, source{}, destination{}, directory{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    Ext4NamespaceEmptyOrphanAuthorization orphan{};

    Fixture(const Image &image, const Ext4RecoveryInfo &info, u32 from = 14, u32 to = 15)
        : disk(image), binding(info), source_number(from), destination_number(to)
    {
        super = load(disk, 1); descriptors = load(disk, 2);
        const auto table_for = [&](u32 inode) {
            const u32 group = (inode - 1) / 2048;
            return value(descriptors, group * 32 + 8) + ((inode - 1) % 2048) / 4;
        };
        parent_block = table_for(parent_number);
        source_block = table_for(source_number);
        destination_block = table_for(destination_number);
        parent = load(disk, parent_block);
        source = load(disk, source_block);
        destination = load(disk, destination_block);
        directory_block = value(parent, slot(parent_number) + 60);
        directory = load(disk, directory_block);
        for (u32 group = 0; group < 4; ++group)
        {
            block_bitmaps[group] = load(disk, value(descriptors, group * 32));
            inode_bitmaps[group] = load(disk, value(descriptors, group * 32 + 4));
        }
        orphan = {binding.uuid, 0, true};
    }
    Ext4NamespaceSource images() const
    {
        return {parent_block, source_block, destination_block, directory_block,
                super, descriptors, parent, source, destination, directory,
                inode_bitmaps, block_bitmaps, orphan};
    }
    Ext4NamespaceUpdate replacement(std::string_view old_name, std::string_view new_name) const
    {
        return {Ext4NamespaceOperation::replace, parent_number, source_number, destination_number,
                value(parent, slot(parent_number) + 100), value(source, slot(source_number) + 100),
                value(destination, slot(destination_number) + 100), old_name, new_name,
                1700000333, 123456789};
    }
    Ext4NamespaceUpdate unlink(std::string_view old_name) const
    {
        return {Ext4NamespaceOperation::unlink, parent_number, source_number, 0,
                value(parent, slot(parent_number) + 100), value(source, slot(source_number) + 100),
                0, old_name, {}, 1700000333, 123456789};
    }
    void stamp_inode(Block &table, u32 number)
    {
        auto inode = std::span<u8>{table}.subspan(slot(number), 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        inode[124] = u8(checksum); inode[125] = u8(checksum >> 8);
        inode[130] = u8(checksum >> 16); inode[131] = u8(checksum >> 24);
    }
    void stamp_directory()
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto inode = std::span<const u8>{parent}.subspan(slot(parent_number), 256);
        store32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, parent_number, inode),
            std::span<const u8>{directory}.first(1012)));
    }
    void stamp_descriptor(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const usize offset = group * 32;
        store16(descriptors, offset + 30, 0);
        const u32 checksum = jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)),
            std::span<const u8>{descriptors}.subspan(offset, 32));
        store16(descriptors, offset + 30, u16(checksum));
    }
    void stamp_inode_bitmap(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        store16(descriptors, group * 32 + 26, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{inode_bitmaps[group]}.first(256))));
        stamp_descriptor(group);
    }
    void stamp_block_bitmap(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        store16(descriptors, group * 32 + 24, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{block_bitmaps[group]})));
        stamp_descriptor(group);
    }
    usize entry(std::string_view name) const
    {
        for (usize offset = 0; offset < 1012; offset += short_value(directory, offset + 4))
            if (value(directory, offset) && directory[offset + 6] == name.size() &&
                std::string_view{reinterpret_cast<const char *>(directory.data() + offset + 8), name.size()} == name)
                return offset;
        return 1012;
    }
    void add_alias(u32 number, std::string_view anchor, std::string_view alias)
    {
        const usize at = entry(anchor);
        CHECK(at < 1012 && alias.size() <= 31);
        const usize minimum = (8 + directory[at + 6] + 3) & ~usize{3};
        const u16 record = short_value(directory, at + 4);
        CHECK(record >= minimum + ((8 + alias.size() + 3) & ~usize{3}));
        store16(directory, at + 4, u16(minimum));
        const usize created = at + minimum;
        store32(directory, created, number);
        store16(directory, created + 4, u16(record - minimum));
        directory[created + 6] = u8(alias.size()); directory[created + 7] = 1;
        for (usize index = created + 8; index < at + record; ++index) directory[index] = 0;
        for (usize index = 0; index < alias.size(); ++index)
            directory[created + 8 + index] = u8(alias[index]);
        stamp_directory();
    }
};

} // namespace namespace_journal_model
