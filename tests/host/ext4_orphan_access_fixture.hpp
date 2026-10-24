#pragma once

#include "ok/fs/ext4_orphan_access.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "orphan access model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4OrphanBlock;
using Image = std::vector<u8>;

class MemoryDisk final : public driver::BlockDevice
{
  public:
    explicit MemoryDisk(const Image &image) : image_(image) {}
    driver::BlockGeometry geometry() const override { return {image_.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > image_.size() / 512 || out.size() % 512 ||
            out.size() / 512 > image_.size() / 512 - sector)
            return Status::invalid_argument("orphan model read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("orphan model must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("orphan model must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &image_;
};

class ImageReader final
{
  public:
    explicit ImageReader(const Image &image) : image_(image) {}
    Status read_block(u32 number, std::span<u8> out)
    {
        if (out.size() < 1024 || number >= image_.size() / 1024)
            return Status::invalid_argument("orphan model block read bounds");
        for (usize index = 0; index < 1024; ++index)
            out[index] = image_[usize{number} * 1024 + index];
        ++reads; return {};
    }
    usize reads{};
  private:
    const Image &image_;
};

Block load(const Image &image, u32 number)
{
    CHECK(number < image.size() / 1024);
    Block result{};
    for (usize index = 0; index < result.size(); ++index)
        result[index] = image[usize{number} * 1024 + index];
    return result;
}
u32 value(const Block &bytes, usize at) { return ext4_checked::le32(bytes, at); }
void store16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void store32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
usize slot(u32 number) { return ((number - 1) % 4) * 256; }

struct Fixture
{
    const Ext4RecoveryInfo &binding;
    Block super{}, descriptors{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<Ext4OrphanInodeImage, 8> images{};
    usize count{};

    Fixture(const Image &image, const Ext4RecoveryInfo &info) : binding(info)
    {
        super = load(image, 1); descriptors = load(image, 2);
        for (u32 group = 0; group < 4; ++group)
        {
            block_bitmaps[group] = load(image, value(descriptors, group * 32));
            inode_bitmaps[group] = load(image, value(descriptors, group * 32 + 4));
        }
    }
    Ext4OrphanChainSource source() const
    { return {super, descriptors, inode_bitmaps, block_bitmaps, std::span{images}.first(count)}; }
    u32 table_for(u32 number) const
    {
        const u32 group = (number - 1) / 2048;
        return value(descriptors, group * 32 + 8) + ((number - 1) % 2048) / 4;
    }
    void stamp_super()
    {
        store32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
    }
    void stamp_inode(Block &table, u32 number)
    {
        auto inode = std::span<u8>{table}.subspan(slot(number), 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(checksum)); store16(inode, 130, u16(checksum >> 16));
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
    void stamp_descriptor(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const usize at = group * 32;
        store16(descriptors, at + 30, 0);
        store16(descriptors, at + 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)),
            std::span<const u8>{descriptors}.subspan(at, 32))));
    }
    void add(const Image &image, u32 number, u32 next)
    {
        CHECK(count < images.size());
        auto &entry = images[count++];
        entry.number = number;
        entry.table_block = table_for(number);
        entry.table = load(image, entry.table_block);
        auto inode = std::span<u8>{entry.table}.subspan(slot(number), 256);
        entry.expected_generation = ext4_checked::le32(inode, 100);
        store16(inode, 26, 0);
        store32(inode, 20, next);
        // The namespace transaction increments i_version before publication.
        store32(inode, 36, ext4_checked::le32(inode, 36) + 1);
        stamp_inode(entry.table, number);
        for (usize prior = 0; prior + 1 < count; ++prior)
        {
            if (images[prior].table_block != entry.table_block) continue;
            auto &earlier = images[prior].table;
            auto copy = std::span<const u8>{earlier}.subspan(slot(images[prior].number), 256);
            for (usize index = 0; index < 256; ++index)
                entry.table[slot(images[prior].number) + index] = copy[index];
            copy = std::span<const u8>{entry.table}.subspan(slot(number), 256);
            for (usize index = 0; index < 256; ++index)
                earlier[slot(number) + index] = copy[index];
        }
    }
    void head(u32 number)
    {
        store32(super, 232, number);
        stamp_super();
    }
};

} // namespace
