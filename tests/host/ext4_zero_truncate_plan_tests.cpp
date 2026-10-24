#include "ext4_zero_truncate_test_support.hpp"
#include <cstdio>
#include <cstdlib>

using namespace zero_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
Ext4ZeroTruncatePlan scratch{}, output{};
usize cases{};

void mapped(Fixture &f, u32 blocks, u32 size, u32 records = 1)
{
    auto inode = std::span<u8>{f.inode_table}.first(256);
    put32(inode, 4, size); put32(inode, 28, blocks * 2);
    for (usize at = 52; at < 100; ++at) inode[at] = 0;
    records = blocks ? (records < blocks ? records : blocks) : 0;
    put16(inode, 42, u16(records));
    u32 logical = 0;
    for (u32 index = 0; index < records; ++index)
    {
        const u32 count = (blocks - logical + records - index - 1) / (records - index);
        const usize at = 52 + index * 12;
        put32(inode, at, logical); put16(inode, at + 4, u16(count));
        put32(inode, at + 8, f.physical + 2 + logical + index * 3);
        logical += count;
    }
    // The retained timestamps deliberately differ from the write event.
    put32(inode, 8, 1699999000); put32(inode, 140, 17U << 2);
    put32(inode, 144, 1600000000); put32(inode, 148, 39U << 2);
    f.refresh_inode();
}

void success(Fixture &f)
{
    const auto original = f;
    const auto update = f.update();
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, update, f.source(), scratch, output));
    CHECK(output.inode == f.inode && output.generation == 9 && output.old_size == update.old_size);
    const u32 blocks = (update.old_size + 1023) / 1024;
    CHECK(output.released_count == blocks && output.noop == (blocks == 0));
    CHECK(f.inode_table == original.inode_table && f.bitmap == original.bitmap &&
          f.inode_bitmap == original.inode_bitmap && f.descriptors == original.descriptors &&
          f.superblock == original.superblock);
    if (!blocks)
    {
        CHECK(output.count == 0);
        CHECK((output.before == std::array<Block, 4>{} && output.after == std::array<Block, 4>{}));
        ++cases; return;
    }
    CHECK(output.count == 4);
    CHECK(output.targets == (std::array<u32, 4>{f.table_block, f.cross ? 8195U : 3U, 2, 1}));
    CHECK(output.before[0] == f.inode_table && output.before[1] == f.bitmap &&
          output.before[2] == f.descriptors && output.before[3] == f.superblock);
    const auto inode = std::span<const u8>{output.after[0]}.first(256);
    CHECK(ext4_checked::le32(inode, 4) == 0 && ext4_checked::le32(inode, 28) == 0);
    CHECK(ext4_checked::le32(inode, 100) == 9 && ext4_checked::le16(inode, 26) == 1);
    CHECK(ext4_checked::le32(inode, 8) == 1699999000 && ext4_checked::le32(inode, 140) == (17U << 2));
    CHECK(ext4_checked::le32(inode, 144) == 1600000000 && ext4_checked::le32(inode, 148) == (39U << 2));
    CHECK(ext4_checked::le32(inode, 12) == update.seconds && ext4_checked::le32(inode, 16) == update.seconds);
    CHECK(ext4_checked::le32(inode, 132) == (update.nanoseconds << 2) &&
          ext4_checked::le32(inode, 136) == (update.nanoseconds << 2));
    const u64 old_version = u64{ext4_checked::le32(f.inode_table, 36)} |
        (u64{ext4_checked::le32(f.inode_table, 152)} << 32);
    CHECK((u64{ext4_checked::le32(inode, 36)} | (u64{ext4_checked::le32(inode, 152)} << 32)) == old_version + 1);
    CHECK(ext4_checked::le16(inode, 40) == 0xf30a && ext4_checked::le16(inode, 44) == 4);
    for (usize at = 40; at < 100; ++at)
        if (at != 40 && at != 41 && at != 44) CHECK(!inode[at]);
    CHECK(ext4_checked::inode_checksum(f.seed, f.inode, inode));
    for (usize at = 0; at < 1024; ++at)
    {
        const bool changed = (at >= 4 && at < 8) || (at >= 12 && at < 20) ||
            (at >= 28 && at < 32) || (at >= 36 && at < 100) ||
            (at >= 124 && at < 126) || (at >= 130 && at < 140) || (at >= 152 && at < 156);
        if (!changed) CHECK(output.after[0][at] == f.inode_table[at]);
    }
    Block expected = f.bitmap;
    for (u32 index = 0; index < blocks; ++index)
    {
        const u32 physical = output.released_mapping[index];
        const u32 local = (physical - 1) % 8192;
        CHECK(expected[local / 8] & (1U << (local % 8)));
        expected[local / 8] &= u8(~(1U << (local % 8)));
    }
    CHECK(output.after[1] == expected);
    const usize d = f.group * 32;
    CHECK(ext4_checked::le16(output.after[2], d + 12) == blocks + 1);
    CHECK(ext4_checked::le16(output.after[2], d + 14) == ext4_checked::le16(f.descriptors, d + 14));
    CHECK(ext4_checked::le32(output.after[3], 12) == blocks + 1);
    CHECK(ext4_checked::le32(output.after[3], 16) == ext4_checked::le32(f.superblock, 16));
    for (usize at = 0; at < 1024; ++at)
    {
        if (at != d + 12 && at != d + 13 && at != d + 24 && at != d + 25 && at != d + 30 && at != d + 31)
            CHECK(output.after[2][at] == f.descriptors[at]);
        if (!(at >= 12 && at < 16) && !(at >= 1020)) CHECK(output.after[3][at] == f.superblock[at]);
    }
    ++cases;
}

void reject(Fixture &f, StatusCode code, Ext4ZeroTruncateUpdate update)
{
    const auto original = f;
    const auto status = Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, update, f.source(), scratch, output);
    CHECK(!status && status.code() == code && output.count == 0);
    CHECK(f.inode_table == original.inode_table && f.bitmap == original.bitmap &&
          f.inode_bitmap == original.inode_bitmap && f.descriptors == original.descriptors &&
          f.superblock == original.superblock);
    ++cases;
}
void reject(Fixture &f, StatusCode code) { reject(f, code, f.update()); }
void negatives()
{
    for (u32 links : {0U, 2U})
    {
        Fixture f; mapped(f, 1, 19); put16(f.inode_table, 26, u16(links)); f.refresh_inode();
        reject(f, StatusCode::unsupported);
    }
    for (u16 mode : {u16{0040640}, u16{0104640}, u16{0102640}, u16{0101640}})
    {
        Fixture f; mapped(f, 1, 19); put16(f.inode_table, 0, mode); f.refresh_inode();
        reject(f, StatusCode::unsupported);
    }
    for (usize at : {usize{20}, usize{104}, usize{108}, usize{112}, usize{156}, usize{160}, usize{200}})
    {
        Fixture f; mapped(f, 1, 19); f.inode_table[at] = 1; f.refresh_inode();
        reject(f, StatusCode::unsupported);
    }
    for (usize at : {usize{46}, usize{48}, usize{58}, usize{76}})
    {
        Fixture f; mapped(f, 1, 19); f.inode_table[at] = 1; f.refresh_inode();
        reject(f, StatusCode::unsupported);
    }
    { Fixture f; mapped(f, 17, 17408); reject(f, StatusCode::unsupported); }
    { Fixture f; mapped(f, 2, 1025); put32(f.inode_table, 52, 1); f.refresh_inode(); reject(f, StatusCode::unsupported); }
    { Fixture f; mapped(f, 1, 19); put16(f.inode_table, 56, 0x8001); f.refresh_inode(); reject(f, StatusCode::unsupported); }
    { Fixture f; mapped(f, 2, 1025, 2); put32(f.inode_table, 72, ext4_checked::le32(f.inode_table, 60)); f.refresh_inode(); reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); put32(f.inode_table, 60, 1000); f.refresh_inode(); reject(f, StatusCode::unsupported); }
    { Fixture f; mapped(f, 1, 19); put32(f.inode_table, 60, 5); f.refresh_inode(); reject(f, StatusCode::unsupported); }
    { Fixture f(true); mapped(f, 1, 19); put32(f.inode_table, 60, 72); f.refresh_inode(); reject(f, StatusCode::unsupported); }
    { Fixture f; mapped(f, 1, 19); f.inode_table[124] ^= 1; reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); f.bitmap[10] ^= 1; reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); f.inode_bitmap[1] ^= 1; reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); f.descriptors[30] ^= 1; reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); f.superblock[1020] ^= 1; reject(f, StatusCode::io_error); }
    { Fixture f; mapped(f, 1, 19); auto u = f.update(); ++u.generation; reject(f, StatusCode::io_error, u); }
    { Fixture f; mapped(f, 1, 19); auto u = f.update(); ++u.old_size; reject(f, StatusCode::io_error, u); }
    { Fixture f; mapped(f, 1, 19); auto u = f.update(); u.nanoseconds = 1000000000; reject(f, StatusCode::invalid_argument, u); }
    { Fixture f; mapped(f, 1, 19); put32(f.inode_table, 36, ~u32{0}); put32(f.inode_table, 152, ~u32{0}); f.refresh_inode(); reject(f, StatusCode::overflow); }
    {
        Fixture f; mapped(f, 1, 19); const u32 local = (ext4_checked::le32(f.inode_table, 60) - 1) % 8192;
        f.bitmap[local / 8] &= u8(~(1U << (local % 8)));
        put16(f.descriptors, 12, 2); put32(f.superblock, 12, 2);
        f.refresh_descriptor(0); f.refresh_super(); reject(f, StatusCode::io_error);
    }
    {
        Fixture f; mapped(f, 1, 19); f.bitmap[0] &= u8(~4U);
        put16(f.descriptors, 12, 2); put32(f.superblock, 12, 2);
        f.refresh_descriptor(0); f.refresh_super(); reject(f, StatusCode::io_error);
    }
    {
        Fixture f; mapped(f, 1, 19); put16(f.descriptors, 12, 2); put32(f.superblock, 12, 2);
        f.refresh_descriptor_crc(0); f.refresh_super(); reject(f, StatusCode::io_error);
    }
    {
        Fixture f; mapped(f, 0, 0); put32(f.inode_table, 52, 1); f.refresh_inode(); reject(f, StatusCode::unsupported);
    }
    {
        Fixture f; mapped(f, 1, 19);
        CHECK(!Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, f.update(), f.source(), scratch, scratch)); ++cases;
        const Ext4ZeroTruncateSource aliased{f.group, f.table_block, scratch.before[0], f.descriptors,
            f.bitmap, f.inode_bitmap, f.inode_table};
        const auto old = scratch.before[0];
        CHECK(!Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, f.update(), aliased, scratch, output));
        CHECK(scratch.before[0] == old); ++cases;
    }
}
} // namespace

int main()
{
    for (bool cross : {false, true})
        for (u32 count = 1; count <= 16; ++count)
            for (u32 records = 1; records <= 4; ++records)
                for (bool partial : {false, true})
                {
                    Fixture f(cross); mapped(f, count, partial ? (count - 1) * 1024 + 1 : count * 1024, records);
                    for (usize at = 256; at < 1024; ++at) f.inode_table[at] = u8(at * 17);
                    success(f);
                }
    for (bool cross : {false, true}) { Fixture f(cross); mapped(f, 0, 0); success(f); }
    { Fixture f; mapped(f, 1, 1023); put32(f.inode_table, 36, ~u32{0}); f.refresh_inode(); success(f); }
    {
        Fixture f; mapped(f, 0, 0); put32(f.inode_table, 36, ~u32{0}); put32(f.inode_table, 152, ~u32{0});
        f.refresh_inode(); auto u = f.update(); u.nanoseconds = ~u32{0};
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, u, f.source(), scratch, output));
        CHECK(output.noop && output.count == 0); ++cases;
    }
    negatives();
    std::printf("PASS hosted zero-truncate exact-image model cases=%zu\n", cases);
}
