#pragma once
#include "ok/fs/ext4/alloc.hpp"
#include "ext4_orphan_access_fixture.hpp"

namespace
{
struct Tables
{
    std::array<std::vector<Block>, 4> groups{};

    Tables(const Image &image, const Fixture &fixture)
    {
        for (u32 group = 0; group < 4; ++group)
        {
            if (ext4_checked::le16(fixture.descriptors, group * 32 + 18) & 1) continue;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            for (u32 offset = 0; offset < 512; ++offset)
                groups[group].push_back(load(image, first + offset));
        }
        for (usize index = 0; index < fixture.count; ++index)
        {
            const auto &entry = fixture.images[index];
            const u32 group = (entry.number - 1) / 2048;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            groups[group][entry.table_block - first] = entry.table;
        }
    }

    Ext4OrphanOwnershipSource source(const Fixture &fixture) const
    {
        std::array<std::span<const Block>, 4> spans{};
        for (usize group = 0; group < groups.size(); ++group)
            spans[group] = groups[group];
        return {fixture.source(), spans};
    }

    std::span<const u8> inode(const Fixture &fixture, u32 number) const
    {
        const u32 group = (number - 1) / 2048;
        const u32 local = (number - 1) % 2048;
        const u32 first = value(fixture.descriptors, group * 32 + 8);
        return std::span<const u8>{groups[group][fixture.table_for(number) - first]}
            .subspan((local % 4) * 256, 256);
    }
};

struct DirectoryImages
{
    std::vector<Ext4OrphanDirectoryBlockImage> blocks{};

    DirectoryImages(const Image &image, const Fixture &fixture, const Tables &tables)
    {
        for (u32 number = 1; number <= 8192; ++number)
        {
            const u32 local = (number - 1) % 2048;
            const u32 group = (number - 1) / 2048;
            if (!(fixture.inode_bitmaps[group][local / 8] & (1U << (local % 8)))) continue;
            const auto inode = tables.inode(fixture, number);
            if ((ext4_checked::le16(inode, 0) & 0170000) != 0040000) continue;
            const u32 size = ext4_checked::le32(inode, 4);
            CHECK(size && size <= 64 * 1024 && size % 1024 == 0);
            for (u32 logical = 0; logical < size / 1024; ++logical)
            {
                u32 physical = 0;
                for (u32 record = 0; record < ext4_checked::le16(inode, 42); ++record)
                {
                    const usize at = 52 + record * 12;
                    const u32 first = ext4_checked::le32(inode, at);
                    const u32 length = ext4_checked::le16(inode, at + 4);
                    if (logical >= first && logical - first < length)
                        physical = ext4_checked::le32(inode, at + 8) + logical - first;
                }
                CHECK(physical);
                blocks.push_back({number, logical, physical, load(image, physical)});
            }
        }
        CHECK(blocks.size() <= Ext4OrphanDirectoryReferenceScratch::block_capacity);
    }

    std::span<const Ext4OrphanDirectoryBlockImage> images() const { return blocks; }

    void stamp(const Fixture &fixture, const Tables &tables, usize index)
    {
        auto &block = blocks[index];
        const u32 seed = jbd2::detail::crc32c(~u32{0}, fixture.binding.uuid);
        const u32 crc = jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, block.inode, tables.inode(fixture, block.inode)),
            std::span<const u8>{block.bytes}.first(1012));
        store32(block.bytes, 1020, crc);
    }

    usize find_name(u32 directory, std::string_view wanted) const
    {
        for (usize index = 0; index < blocks.size(); ++index)
        {
            if (blocks[index].inode != directory) continue;
            const auto &bytes = blocks[index].bytes;
            for (usize at = 0; at < 1012; at += ext4_checked::le16(bytes, at + 4))
            {
                CHECK(ext4_checked::le16(bytes, at + 4));
                const std::string_view name{
                    reinterpret_cast<const char *>(bytes.data() + at + 8), bytes[at + 6]};
                if (name == wanted) return (index << 16) | at;
            }
        }
        std::fprintf(stderr, "missing fixture name: %.*s\n", int(wanted.size()), wanted.data());
        std::abort();
    }

    void remove_target(const Fixture &fixture, const Tables &tables, u32 number)
    {
        bool removed = false;
        for (usize index = 0; index < blocks.size(); ++index)
        {
            auto &image = blocks[index];
            for (usize at = 0; at < 1012; at += ext4_checked::le16(image.bytes, at + 4))
            {
                CHECK(ext4_checked::le16(image.bytes, at + 4));
                if (ext4_checked::le32(image.bytes, at) != number) continue;
                store32(image.bytes, at, 0);
                stamp(fixture, tables, index);
                removed = true;
            }
        }
        CHECK(removed);
    }
};

Image read_image(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream);
    Image image((std::istreambuf_iterator<char>(stream)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    return image;
}

} // namespace
