#include <ok/fs/ext4/extent.hpp>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "next data plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4NextDataBlock;
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
void stamp_descriptor(std::span<u8> descriptor, u32 number, u32 seed)
{
    put16(descriptor, 30, 0);
    put16(descriptor, 30, u16(jbd2::detail::crc32c(
        jbd2::detail::crc32c(seed, ext4_checked::encoded(number)), descriptor)));
}

struct Extent { u32 physical{}; u16 count{}; };
struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block superblock{}, descriptors{}, bitmap{}, inode_table{};
    u32 group{}, candidate{}, inode{13}, table_block{8}, old_blocks{}, seed{};

    explicit Fixture(bool cross = false)
    {
        group = cross ? 1 : 0;
        candidate = cross ? 8262 : 70;
        binding.layout.block_size = 1024;
        binding.layout.filesystem_blocks = cross ? 8705 : 8193;
        binding.layout.journal_first_block = 1000;
        binding.layout.journal_blocks = 1024;
        binding.metadata_count = cross ? 8 : 4;
        binding.metadata[0] = {1, 2}; binding.metadata[1] = {3, 1};
        binding.metadata[2] = {4, 1}; binding.metadata[3] = {5, 64};
        if (cross)
        {
            binding.metadata[4] = {8193, 2}; binding.metadata[5] = {8195, 1};
            binding.metadata[6] = {8196, 1}; binding.metadata[7] = {8197, 64};
        }
        binding.journal_inode = 8;
        binding.needs_recovery = true; binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 1);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        bitmap.fill(0xff);
        const u32 local = (candidate - 1) % 8192;
        bitmap[local / 8] &= u8(~(1U << (local % 8)));
        put32(superblock, 0, cross ? 512 : 256);
        put32(superblock, 4, u32(binding.layout.filesystem_blocks));
        put32(superblock, 12, 1); put32(superblock, 20, 1);
        put32(superblock, 32, 8192); put32(superblock, 36, 8192);
        put32(superblock, 40, 256);
        put16(superblock, 56, 0xef53); put16(superblock, 58, 1);
        put32(superblock, 76, 1); put32(superblock, 84, 11);
        put16(superblock, 88, 256); put32(superblock, 92, 4);
        put32(superblock, 96, 0x46); put32(superblock, 100, 0x400);
        for (usize index = 0; index < binding.uuid.size(); ++index)
            superblock[104 + index] = binding.uuid[index];
        put32(superblock, 224, 8); put16(superblock, 254, 32); superblock[373] = 1;
        refresh_super();
        put32(descriptors, 0, 3); put32(descriptors, 4, 4); put32(descriptors, 8, 5);
        put16(descriptors, 12, cross ? 0 : 1); put16(descriptors, 18, 4);
        if (cross)
        {
            put32(descriptors, 32, 8195); put32(descriptors, 36, 8196);
            put32(descriptors, 40, 8197); put16(descriptors, 44, 1); put16(descriptors, 50, 4);
        }
        refresh_descriptor(0);
        if (cross) refresh_descriptor(1);
        extents({{cross ? 90U : 69U, 1}});
    }
    void refresh_super()
    { put32(superblock, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{superblock}.first(1020))); }
    void refresh_descriptor(u32 number)
    {
        const Block all_set = [] { Block value{}; value.fill(0xff); return value; }();
        const Block &bits = number == group ? bitmap : all_set;
        put16(descriptors, number * 32 + 24, u16(jbd2::detail::crc32c(seed, bits)));
        stamp_descriptor(std::span<u8>{descriptors}.subspan(number * 32, 32), number, seed);
    }
    void refresh_inode()
    { stamp_inode(std::span<u8>{inode_table}.first(256), inode, seed); }
    void extents(std::initializer_list<Extent> extents)
    {
        inode_table = {};
        auto node = std::span<u8>{inode_table}.first(256);
        put16(node, 0, 0100640); put16(node, 2, 1000); put16(node, 120, 1);
        put16(node, 24, 1001); put16(node, 122, 2); put16(node, 26, 1);
        put32(node, 8, 1700000000); put32(node, 140, 123456789U << 2);
        put32(node, 12, 1700000222); put32(node, 132, 222222222U << 2);
        put32(node, 16, 1700000333); put32(node, 136, 333333333U << 2);
        put32(node, 144, 1700000000); put32(node, 148, 123456789U << 2);
        put32(node, 32, 0x80000);
        put32(node, 36, 1);
        put16(node, 40, 0xf30a); put16(node, 42, u16(extents.size()));
        put16(node, 44, 4); put16(node, 128, 32);
        old_blocks = 0;
        usize index = 0;
        for (const auto extent : extents)
        {
            CHECK(index < 4);
            const usize at = 52 + index * 12;
            put32(node, at, old_blocks); put16(node, at + 4, extent.count);
            put32(node, at + 8, extent.physical);
            old_blocks += extent.count;
            ++index;
        }
        put32(node, 4, old_blocks * 1024); put32(node, 28, old_blocks * 2);
        refresh_inode();
    }
    [[nodiscard]] Ext4NextDataUpdate update(u32 added = 19) const
    { return {inode, old_blocks * 1024, old_blocks * 1024 + added, 1700000444, 444444444}; }
    [[nodiscard]] Ext4NextDataSource source() const
    { return {group, candidate, table_block, superblock, descriptors, bitmap, inode_table}; }
};

void success(bool cross, bool contiguous)
{
    Fixture fixture(cross);
    fixture.extents({{contiguous ? fixture.candidate - 1 : 90, 1}});
    Ext4NextDataPlan plan{};
    const auto status = Ext4NextDataPlanner::plan(fixture.binding, fixture.update(), fixture.source(), plan);
    if (!status) std::fprintf(stderr, "plan code=%u reason=%.*s\n", u32(status.code()),
                              int(status.message().size()), status.message().data());
    CHECK(status);
    CHECK(plan.count == 4 && plan.inode == fixture.inode && plan.physical == fixture.candidate);
    CHECK(plan.old_blocks == 1 && plan.old_physical[0] == (contiguous ? fixture.candidate - 1 : 90));
    CHECK(plan.targets[0] == (cross ? 8195U : 3U) && plan.targets[1] == 2 &&
          plan.targets[2] == 1 && plan.targets[3] == fixture.table_block);
    CHECK(plan.before[0] == fixture.bitmap && plan.before[1] == fixture.descriptors &&
          plan.before[2] == fixture.superblock && plan.before[3] == fixture.inode_table);
    CHECK(ext4_checked::le32(plan.after[2], 12) == 0);
    CHECK(ext4_checked::le32(plan.after[3], 4) == fixture.old_blocks * 1024 + 19);
    CHECK(ext4_checked::le32(plan.after[3], 28) == 4);
    CHECK(ext4_checked::le32(plan.after[3], 36) == 2);
    CHECK(ext4_checked::le32(plan.after[3], 8) == 1700000000);
    CHECK(ext4_checked::le32(plan.after[3], 144) == 1700000000);
    CHECK(ext4_checked::le32(plan.after[3], 12) == 1700000444);
    CHECK(ext4_checked::le32(plan.after[3], 16) == 1700000444);
    CHECK(ext4_checked::le16(plan.after[3], 42) == (contiguous ? 1 : 2));
    CHECK(ext4_checked::le16(plan.after[3], 56) == (contiguous ? 2 : 1));
    CHECK(ext4_checked::inode_checksum(fixture.seed, fixture.inode,
          std::span<const u8>{plan.after[3]}.first(256)));
    Ext4NextDataPrefix prefix{};
    CHECK(Ext4NextDataPlanner::inspect_final_inode(fixture.binding, fixture.inode,
          plan.after[3], 0, fixture.candidate, prefix));
    CHECK(prefix.old_blocks == plan.old_blocks && prefix.old_physical == plan.old_physical);
    CHECK(fixture.bitmap == plan.before[0] && fixture.inode_table == plan.before[3]);
}

void capacity_and_rejects()
{
    auto reject = [](Fixture &fixture, StatusCode code) {
        Ext4NextDataPlan plan{};
        const auto result = Ext4NextDataPlanner::plan(fixture.binding, fixture.update(), fixture.source(), plan);
        CHECK(!result && result.code() == code && plan.count == 0);
    };
    Fixture fourth;
    fourth.extents({{90, 1}, {92, 1}, {94, 1}});
    Ext4NextDataPlan plan{};
    CHECK(Ext4NextDataPlanner::plan(fourth.binding, fourth.update(), fourth.source(), plan));
    CHECK(ext4_checked::le16(plan.after[3], 42) == 4);
    Fixture fifth;
    fifth.extents({{90, 1}, {92, 1}, {94, 1}, {96, 1}});
    reject(fifth, StatusCode::unsupported);
    Fixture maximum;
    maximum.extents({{90, 15}});
    CHECK(Ext4NextDataPlanner::plan(maximum.binding, maximum.update(1024), maximum.source(), plan));
    CHECK(ext4_checked::le32(plan.after[3], 4) == 16384);
    CHECK(plan.old_blocks == 15);
    Fixture noncanonical;
    CHECK(Ext4NextDataPlanner::plan(noncanonical.binding, noncanonical.update(),
          noncanonical.source(), plan));
    auto split = plan.after[3];
    put16(split, 42, 2); put16(split, 56, 1);
    put32(split, 64, 1); put16(split, 68, 1); put32(split, 72, noncanonical.candidate);
    stamp_inode(std::span<u8>{split}.first(256), noncanonical.inode, noncanonical.seed);
    Ext4NextDataPrefix prefix{};
    CHECK(Ext4NextDataPlanner::inspect_final_inode(noncanonical.binding, noncanonical.inode,
          split, 0, noncanonical.candidate, prefix).code() == StatusCode::unsupported);
    auto mismatched_clock = plan.after[3];
    put32(mismatched_clock, 16, 1700000555);
    stamp_inode(std::span<u8>{mismatched_clock}.first(256), noncanonical.inode, noncanonical.seed);
    CHECK(Ext4NextDataPlanner::inspect_final_inode(noncanonical.binding, noncanonical.inode,
          mismatched_clock, 0, noncanonical.candidate, prefix).code() == StatusCode::unsupported);
    auto invalid_final_nanos = plan.after[3];
    put32(invalid_final_nanos, 136, 1000000000U << 2);
    stamp_inode(std::span<u8>{invalid_final_nanos}.first(256), noncanonical.inode, noncanonical.seed);
    CHECK(Ext4NextDataPlanner::inspect_final_inode(noncanonical.binding, noncanonical.inode,
          invalid_final_nanos, 0, noncanonical.candidate, prefix).code() == StatusCode::unsupported);
    auto zero_final_version = plan.after[3];
    put32(zero_final_version, 36, 0); put32(zero_final_version, 152, 0);
    stamp_inode(std::span<u8>{zero_final_version}.first(256), noncanonical.inode, noncanonical.seed);
    CHECK(Ext4NextDataPlanner::inspect_final_inode(noncanonical.binding, noncanonical.inode,
          zero_final_version, 0, noncanonical.candidate, prefix).code() == StatusCode::unsupported);
    Fixture carry;
    put32(carry.inode_table, 36, 0xffffffffU);
    put32(carry.inode_table, 152, 17);
    carry.refresh_inode();
    CHECK(Ext4NextDataPlanner::plan(carry.binding, carry.update(), carry.source(), plan));
    CHECK(ext4_checked::le32(plan.after[3], 36) == 0);
    CHECK(ext4_checked::le32(plan.after[3], 152) == 18);
    Fixture exhausted_version;
    put32(exhausted_version.inode_table, 36, 0xffffffffU);
    put32(exhausted_version.inode_table, 152, 0xffffffffU);
    exhausted_version.refresh_inode();
    reject(exhausted_version, StatusCode::overflow);
    Fixture wrong_size;
    put32(wrong_size.inode_table, 4, 777); wrong_size.refresh_inode();
    reject(wrong_size, StatusCode::unsupported);
    Fixture hole;
    put32(hole.inode_table, 52, 1); hole.refresh_inode();
    reject(hole, StatusCode::unsupported);
    Fixture unwritten;
    put16(unwritten.inode_table, 56, 32769); unwritten.refresh_inode();
    reject(unwritten, StatusCode::unsupported);
    Fixture candidate_alias;
    candidate_alias.extents({{candidate_alias.candidate, 1}});
    reject(candidate_alias, StatusCode::io_error);
    Fixture reserved;
    reserved.extents({{1000, 1}});
    reject(reserved, StatusCode::unsupported);
    Fixture bad_crc;
    bad_crc.inode_table[124] ^= 1;
    reject(bad_crc, StatusCode::io_error);
    Fixture bad_bitmap;
    bad_bitmap.bitmap[8] ^= 0x20;
    reject(bad_bitmap, StatusCode::io_error);
    Fixture bad_counter;
    put16(bad_counter.descriptors, 12, 2); stamp_descriptor(
        std::span<u8>{bad_counter.descriptors}.first(32), 0, bad_counter.seed);
    put32(bad_counter.superblock, 12, 2); bad_counter.refresh_super();
    reject(bad_counter, StatusCode::io_error);
    Fixture wrong_physical;
    wrong_physical.candidate = 71;
    reject(wrong_physical, StatusCode::invalid_argument);
    Fixture bad_clock;
    put32(bad_clock.inode_table, 132, 1000000000U << 2); bad_clock.refresh_inode();
    reject(bad_clock, StatusCode::unsupported);
    Fixture negative_old_time;
    put32(negative_old_time.inode_table, 12, 0x80000000U);
    negative_old_time.refresh_inode();
    reject(negative_old_time, StatusCode::unsupported);
    Fixture special;
    put16(special.inode_table, 0, 0104640); special.refresh_inode();
    reject(special, StatusCode::unsupported);
    Fixture bad_update;
    auto update = bad_update.update(); update.old_size = 0;
    CHECK(Ext4NextDataPlanner::plan(bad_update.binding, update, bad_update.source(), plan).code() ==
          StatusCode::invalid_argument);
    update = bad_update.update(); update.size = update.old_size + 1025;
    CHECK(Ext4NextDataPlanner::plan(bad_update.binding, update, bad_update.source(), plan).code() ==
          StatusCode::invalid_argument);
    update = bad_update.update(); update.nanoseconds = 1000000000U;
    CHECK(Ext4NextDataPlanner::plan(bad_update.binding, update, bad_update.source(), plan).code() ==
          StatusCode::invalid_argument);
}
} // namespace

int main()
{
    success(false, true); success(false, false);
    success(true, true); success(true, false);
    capacity_and_rejects();
    std::puts("PASS next data image plan model: extents, cross-group, four homes, checksums and rejects");
}
