#include <ok/fs/ext4/extent.hpp>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "first data plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4FirstDataBlock;

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
    Block superblock{}, descriptors{}, bitmap{}, other_bitmap{}, inode_table{};
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
        put32(superblock, 12, 1);
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
        put16(descriptors, 18, 4);
        if (cross)
        {
            put32(descriptors, 32, 8195); put32(descriptors, 36, 8196);
            put32(descriptors, 40, 8197); put16(descriptors, 44, 1);
            put16(descriptors, 50, 4);
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
    [[nodiscard]] Ext4FirstDataSource source() const
    { return {group, physical, table_block, superblock, descriptors, bitmap, inode_table}; }
    [[nodiscard]] Ext4FirstDataUpdate update() const
    { return {inode, 19, 1700000111, 765432100}; }
};

void success_and_exact_images(bool cross)
{
    Fixture fixture(cross);
    const auto chosen = Ext4FirstDataPlanner::select_block(
        fixture.binding, fixture.superblock, fixture.descriptors, fixture.bitmap, fixture.group);
    CHECK(chosen && chosen.value() == fixture.physical);
    Ext4FirstDataPlan plan{};
    CHECK(Ext4FirstDataPlanner::plan(fixture.binding, fixture.update(), fixture.source(), plan));
    CHECK(plan.count == 4 && plan.inode == fixture.inode && plan.physical == fixture.physical);
    CHECK(plan.targets[0] == (cross ? 8195U : 3U) && plan.targets[1] == 2 &&
          plan.targets[2] == 1 && plan.targets[3] == fixture.table_block);
    CHECK(plan.before[0] == fixture.bitmap && plan.before[1] == fixture.descriptors &&
          plan.before[2] == fixture.superblock && plan.before[3] == fixture.inode_table);
    const u32 local = (fixture.physical - 1) % 8192;
    Block expected_bitmap = fixture.bitmap;
    expected_bitmap[local / 8] |= u8(1U << (local % 8));
    CHECK(plan.after[0] == expected_bitmap);
    Block expected_descriptor = fixture.descriptors;
    const usize d = fixture.group * 32;
    put16(expected_descriptor, d + 12, 0);
    put16(expected_descriptor, d + 24, u16(jbd2::detail::crc32c(
        fixture.seed, std::span<const u8>{expected_bitmap}.first(1024))));
    stamp_descriptor(std::span<u8>{expected_descriptor}.subspan(d, 32), fixture.group, fixture.seed);
    CHECK(plan.after[1] == expected_descriptor);
    Block expected_super = fixture.superblock;
    put32(expected_super, 12, 0);
    put32(expected_super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{expected_super}.first(1020)));
    CHECK(plan.after[2] == expected_super);
    Block expected_table = fixture.inode_table;
    auto node = std::span<u8>{expected_table}.first(256);
    put32(node, 4, 19);
    put32(node, 12, 1700000111); put32(node, 132, 765432100U << 2);
    put32(node, 16, 1700000111); put32(node, 136, 765432100U << 2);
    put32(node, 28, 2); put32(node, 36, 1);
    put16(node, 42, 1); put16(node, 56, 1); put32(node, 60, fixture.physical);
    stamp_inode(node, fixture.inode, fixture.seed);
    CHECK(plan.after[3] == expected_table);
    CHECK(ext4_checked::inode_checksum(fixture.seed, fixture.inode,
          std::span<const u8>{plan.after[3]}.first(256)));
    CHECK(ext4_checked::le16(plan.after[3], 0) == 0100640);
    CHECK(ext4_checked::le16(plan.after[3], 2) == 1000 &&
          ext4_checked::le16(plan.after[3], 120) == 1 &&
          ext4_checked::le16(plan.after[3], 24) == 1001 &&
          ext4_checked::le16(plan.after[3], 122) == 2);
    CHECK(ext4_checked::le32(plan.after[3], 8) == 1700000000 &&
          ext4_checked::le32(plan.after[3], 144) == 1700000000);
    CHECK(fixture.bitmap == plan.before[0] && fixture.inode_table == plan.before[3]);
}

void rejects()
{
    auto reject = [](Fixture &fixture, StatusCode code) {
        Ext4FirstDataPlan output{};
        const auto status = Ext4FirstDataPlanner::plan(fixture.binding, fixture.update(), fixture.source(), output);
        CHECK(!status && status.code() == code && output.count == 0);
    };
    Fixture bad_super;
    bad_super.superblock[1020] ^= 1;
    reject(bad_super, StatusCode::io_error);
    Fixture bad_descriptor;
    bad_descriptor.descriptors[30] ^= 1;
    reject(bad_descriptor, StatusCode::io_error);
    Fixture relocated_bitmap;
    put32(relocated_bitmap.descriptors, 0, 9000);
    relocated_bitmap.refresh_descriptor_crc(0);
    reject(relocated_bitmap, StatusCode::unsupported);
    Fixture bad_bitmap;
    bad_bitmap.bitmap[8] ^= 0x20;
    reject(bad_bitmap, StatusCode::io_error);
    Fixture bad_total;
    put32(bad_total.superblock, 12, 2); bad_total.refresh_super();
    reject(bad_total, StatusCode::io_error);
    Fixture bad_count;
    put16(bad_count.descriptors, 12, 2); bad_count.refresh_descriptor_crc(0);
    put32(bad_count.superblock, 12, 2); bad_count.refresh_super();
    reject(bad_count, StatusCode::io_error);
    Fixture reserved;
    reserved.bitmap[0] &= u8(~(1U << 2));
    put16(reserved.descriptors, 12, 2); reserved.refresh_descriptor(0);
    put32(reserved.superblock, 12, 2); reserved.refresh_super();
    reject(reserved, StatusCode::io_error);
    Fixture journal;
    const u32 journal_local = 999;
    journal.bitmap[journal_local / 8] &= u8(~(1U << (journal_local % 8)));
    put16(journal.descriptors, 12, 2); journal.refresh_descriptor(0);
    put32(journal.superblock, 12, 2); journal.refresh_super();
    reject(journal, StatusCode::io_error);
    Fixture padding(true);
    padding.bitmap[512 / 8] &= u8(~1U);
    padding.refresh_descriptor(1);
    reject(padding, StatusCode::io_error);

    Fixture exhausted;
    exhausted.bitmap.fill(0xff);
    put16(exhausted.descriptors, 12, 0); exhausted.refresh_descriptor(0);
    put32(exhausted.superblock, 12, 0); exhausted.refresh_super();
    CHECK(Ext4FirstDataPlanner::select_block(exhausted.binding, exhausted.superblock,
          exhausted.descriptors, exhausted.bitmap, 0).status().code() == StatusCode::no_space);
    reject(exhausted, StatusCode::no_space);
    Fixture uninitialized(true);
    put16(uninitialized.descriptors, 50, 6);
    put16(uninitialized.descriptors, 44, 444);
    put16(uninitialized.descriptors, 56, 0);
    uninitialized.refresh_descriptor_crc(1);
    put32(uninitialized.superblock, 12, 444); uninitialized.refresh_super();
    CHECK(Ext4FirstDataPlanner::select_block(uninitialized.binding, uninitialized.superblock,
          uninitialized.descriptors, uninitialized.bitmap, 1).status().code() == StatusCode::unsupported);
    reject(uninitialized, StatusCode::unsupported);
    Fixture wrong_group(true);
    put16(wrong_group.descriptors, 12, 1); wrong_group.refresh_descriptor_crc(0);
    put32(wrong_group.superblock, 12, 2); wrong_group.refresh_super();
    reject(wrong_group, StatusCode::invalid_argument);

    Fixture checksum_inode;
    checksum_inode.inode_table[124] ^= 1;
    reject(checksum_inode, StatusCode::io_error);
    Fixture nonempty;
    put32(nonempty.inode_table, 4, 1); nonempty.refresh_inode();
    reject(nonempty, StatusCode::unsupported);
    Fixture high_size;
    put32(high_size.inode_table, 108, 1); high_size.refresh_inode();
    reject(high_size, StatusCode::unsupported);
    Fixture stale_clock;
    put32(stale_clock.inode_table, 12, 1699999999); stale_clock.refresh_inode();
    reject(stale_clock, StatusCode::unsupported);
    Fixture altered_extent;
    put16(altered_extent.inode_table, 56, 1); altered_extent.refresh_inode();
    reject(altered_extent, StatusCode::unsupported);
    Fixture special_mode;
    put16(special_mode.inode_table, 0, 0104640); special_mode.refresh_inode();
    reject(special_mode, StatusCode::unsupported);
    Fixture uninitialized_inode;
    put16(uninitialized_inode.descriptors, 18, 5);
    put16(uninitialized_inode.descriptors, 14, 256);
    put16(uninitialized_inode.descriptors, 28, 256);
    put16(uninitialized_inode.descriptors, 26, 0);
    uninitialized_inode.refresh_descriptor_crc(0);
    reject(uninitialized_inode, StatusCode::unsupported);
    Fixture wrong_table;
    wrong_table.table_block = 9;
    reject(wrong_table, StatusCode::unsupported);
    Fixture wrong_physical;
    wrong_physical.physical = 71;
    reject(wrong_physical, StatusCode::invalid_argument);

    Fixture invalid;
    auto update = invalid.update();
    Ext4FirstDataPlan output{};
    update.size = 0;
    CHECK(Ext4FirstDataPlanner::plan(invalid.binding, update, invalid.source(), output).code() ==
          StatusCode::invalid_argument);
    update.size = 1025;
    CHECK(Ext4FirstDataPlanner::plan(invalid.binding, update, invalid.source(), output).code() ==
          StatusCode::invalid_argument);
    update = invalid.update(); update.nanoseconds = 1000000000;
    CHECK(Ext4FirstDataPlanner::plan(invalid.binding, update, invalid.source(), output).code() ==
          StatusCode::invalid_argument);
}
} // namespace

int main()
{
    success_and_exact_images(false);
    success_and_exact_images(true);
    rejects();
    std::puts("PASS first data image plan model: cross-group allocation, exact four homes, checksums and rejects");
}
