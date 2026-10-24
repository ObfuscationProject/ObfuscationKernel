#include "ext4_zero_truncate_test_support.hpp"
#include <cstdio>
#include <cstdlib>

using namespace zero_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate ownership model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
usize cases{};
Ext4ZeroTruncateOwnershipScratch workspace{};
Ext4ZeroTruncateOwnership proof{};
std::array<Block, 8193> disk{}, saved{};
struct Image
{
    Fixture f;
    std::array<Block, 8193> &blocks{disk};
    explicit Image(u32 count = 1)
    {
        blocks.fill(Block{});
        blocks[1] = f.superblock; blocks[2] = f.descriptors;
        blocks[3] = f.bitmap; blocks[4] = f.inode_bitmap; blocks[8] = f.inode_table;
        for (u32 number : {2U, 8U}) blocks[4][(number - 1) / 8] |= u8(1U << ((number - 1) % 8));
        const auto make = [&](u32 number, u16 mode, u32 size, u32 length, u32 physical, u16 links)
        {
            auto raw = inode(number);
            for (auto &byte : raw) byte = 0;
            put16(raw, 0, mode); put16(raw, 26, links);
            put32(raw, 4, size); put32(raw, 28, length * 2); put32(raw, 32, 0x80000);
            put16(raw, 40, 0xf30a); put16(raw, 42, length ? 1 : 0); put16(raw, 44, 4);
            put16(raw, 56, u16(length)); put32(raw, 60, physical); put16(raw, 128, 32);
            stamp_inode(raw, number, f.seed);
        };
        make(2, 0040755, 1024, 1, 71, 2);
        make(8, 0100600, 1048576, 1024, 1000, 1);
        make(13, 0100640, count ? (count - 1) * 1024 + 1 : 0, count, count ? 72 : 0, 1);
        put32(inode(13), 100, 9); put32(inode(13), 36, 42);
        stamp_inode(inode(13), 13, f.seed);
        auto &directory = blocks[71];
        put32(directory, 0, 2); put16(directory, 4, 12); directory[6] = 1; directory[7] = 2; directory[8] = '.';
        put32(directory, 12, 2); put16(directory, 16, 1000); directory[18] = 2; directory[19] = 2;
        directory[20] = directory[21] = '.';
        put16(directory, 1016, 12); directory[1019] = 0xde;
        put32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(f.seed, 2, inode(2)), std::span<const u8>{directory}.first(1012)));
        refresh();
    }
    std::span<u8, 256> inode(u32 number)
    { return std::span<u8, 256>{blocks[5 + (number - 1) / 4].data() + ((number - 1) % 4) * 256, 256}; }
    void refresh()
    {
        u32 free = 0;
        for (u32 bit = 0; bit < 256; ++bit)
            if (!(blocks[4][bit / 8] & (1U << (bit % 8)))) ++free;
        put16(blocks[2], 14, u16(free)); put16(blocks[2], 16, 1);
        put32(blocks[1], 16, free);
        put16(blocks[2], 26, u16(jbd2::detail::crc32c(f.seed, std::span<const u8>{blocks[4]}.first(32))));
        free = 0;
        for (u32 bit = 0; bit < 8192; ++bit)
            if (!(blocks[3][bit / 8] & (1U << (bit % 8)))) ++free;
        put16(blocks[2], 12, u16(free)); put32(blocks[1], 12, free);
        put16(blocks[2], 24, u16(jbd2::detail::crc32c(f.seed, blocks[3])));
        stamp_descriptor(std::span<u8>{blocks[2]}.first(32), 0, f.seed);
        put32(blocks[1], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{blocks[1]}.first(1020)));
    }
    void duplicate(u32 number, bool direct = false)
    {
        const auto source = inode(13); auto target = inode(number);
        for (usize at = 0; at < 256; ++at) target[at] = source[at];
        if (direct)
        {
            put16(target, 0, 0120777); put32(target, 4, 60); put32(target, 32, 0);
            for (usize at = 40; at < 100; ++at) target[at] = 0;
            put32(target, 40, 72);
        }
        stamp_inode(target, number, f.seed);
        blocks[4][(number - 1) / 8] |= u8(1U << ((number - 1) % 8)); refresh();
    }
    Ext4ZeroTruncateUpdate update()
    { return {13, 9, ext4_checked::le32(inode(13), 4), 1, 0}; }
};
struct Reader
{
    Image &image;
    usize reads{}, fail_at{}, target_reads{}, change_at{};
    Status read_block(u32 number, std::span<u8> out)
    {
        if (++reads == fail_at) return Status::io_error("injected ownership read failure");
        if (number >= image.blocks.size() || out.size() < 1024) return Status::io_error("ownership model bounds");
        if (number == 8 && ++target_reads == change_at)
        {
            put32(image.inode(13), 100, 10); stamp_inode(image.inode(13), 13, image.f.seed);
        }
        for (usize at = 0; at < 1024; ++at) out[at] = image.blocks[number][at];
        return {};
    }
};
using View = Ext4ReadOnlyView<Reader>;

void run(Image &image, StatusCode expected = StatusCode::ok)
{
    Reader reader{image}; View view;
    CHECK(view.open(reader, image.f.binding));
    saved = image.blocks;
    const auto status = Ext4ZeroTruncateOwnershipValidator::prove(image.f.binding, view, image.update(), workspace, proof);
    CHECK(status.code() == expected);
    if (status)
    {
        CHECK(proof.inode == 13 && proof.generation == 9 && proof.old_size == image.update().old_size);
        const u32 count = proof.old_size ? (proof.old_size + 1023) / 1024 : 0;
        CHECK(proof.count == count && proof.group == 0);
        for (u32 index = 0; index < count; ++index) CHECK(proof.mapping[index] == 72 + index);
    }
    else CHECK(!proof.count && !proof.inode);
    CHECK(image.blocks == saved); ++cases;
}
} // namespace

int main()
{
    for (u32 count = 0; count <= 16; ++count) { Image image(count); run(image); }
    for (bool direct : {false, true}) { Image image; image.duplicate(14, direct); run(image, StatusCode::io_error); }
    {
        Image image; image.blocks[3][71 / 8] &= u8(~(1U << (71 % 8))); image.refresh();
        run(image, StatusCode::io_error);
    }
    {
        Image image; image.blocks[3][2 / 8] &= u8(~(1U << (2 % 8))); image.refresh();
        run(image, StatusCode::io_error);
    }
    {
        Image image; put16(image.blocks[2], 12, 2); put32(image.blocks[1], 12, 2);
        stamp_descriptor(std::span<u8>{image.blocks[2]}.first(32), 0, image.f.seed);
        put32(image.blocks[1], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{image.blocks[1]}.first(1020)));
        run(image, StatusCode::io_error);
    }
    {
        Image image; put32(image.blocks[1], 16, 252);
        put32(image.blocks[1], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{image.blocks[1]}.first(1020)));
        run(image, StatusCode::io_error);
    }
    {
        Image image; put32(image.inode(8), 60, 72); stamp_inode(image.inode(8), 8, image.f.seed);
        run(image, StatusCode::unsupported);
    }
    {
        Image image; image.blocks[4][0] |= 4; image.inode(3)[40] = 72; image.refresh();
        run(image, StatusCode::unsupported);
    }
    {
        Image image; Reader reader{image}; View view; CHECK(view.open(reader, image.f.binding));
        put32(image.inode(2), 60, 72); stamp_inode(image.inode(2), 2, image.f.seed);
        saved = image.blocks;
        CHECK(!Ext4ZeroTruncateOwnershipValidator::prove(image.f.binding, view, image.update(), workspace, proof));
        CHECK(image.blocks == saved); ++cases;
    }
    {
        Image image(0);
        for (u32 number = 11; number <= 140; ++number) if (number != 13) image.duplicate(number);
        run(image, StatusCode::no_memory);
    }
    {
        Image image; Reader reader{image}; View view; CHECK(view.open(reader, image.f.binding));
        reader.fail_at = reader.reads + 1;
        CHECK(!Ext4ZeroTruncateOwnershipValidator::prove(image.f.binding, view, image.update(), workspace, proof));
        CHECK(!proof.count && !view.valid()); ++cases;
    }
    {
        Image image; Reader reader{image}; View view; CHECK(view.open(reader, image.f.binding));
        reader.target_reads = 0; reader.change_at = 4;
        const auto status = Ext4ZeroTruncateOwnershipValidator::prove(image.f.binding, view, image.update(), workspace, proof);
        CHECK(!status && status.code() == StatusCode::io_error && !proof.count && !view.valid()); ++cases;
    }
    {
        Image image; Reader reader{image}; View view; CHECK(view.open(reader, image.f.binding));
        auto update = image.update(); ++update.generation;
        CHECK(!Ext4ZeroTruncateOwnershipValidator::prove(image.f.binding, view, update, workspace, proof));
        CHECK(!proof.count); ++cases;
    }
    std::printf("PASS hosted zero-truncate ownership model cases=%zu\n", cases);
}
