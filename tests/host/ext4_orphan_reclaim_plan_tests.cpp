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
            groups[group].reserve(512);
            for (u32 index = 0; index < 512; ++index)
                groups[group].push_back(load(image, first + index));
        }
        for (usize index = 0; index < fixture.count; ++index)
        {
            const auto &member = fixture.images[index];
            const u32 group = (member.number - 1) / 2048;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            groups[group][member.table_block - first] = member.table;
        }
    }
    Ext4OrphanOwnershipSource source(const Fixture &fixture) const
    {
        std::array<std::span<const Block>, 4> spans{};
        for (usize group = 0; group < groups.size(); ++group)
            spans[group] = groups[group];
        return {fixture.source(), spans};
    }
    Block &inode_table(u32 number)
    {
        const u32 group = (number - 1) / 2048;
        return groups[group][(number - 1) % 2048 / 4];
    }
};

Ext4OrphanReclaimPlan make(const Ext4RecoveryInfo &binding, const Fixture &fixture,
                           const Tables &tables, u32 head, u32 generation)
{
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanReclaimPlan plan{};
    CHECK(Ext4OrphanReclaimPlanner::plan(
        binding, tables.source(fixture), scratch, head, generation, plan));
    CHECK(plan.inode == head && plan.old_generation == generation &&
          plan.retained_generation == generation + 1 &&
          plan.count == (plan.released_blocks ? 5U : 4U));
    for (usize index = 0; index < plan.count; ++index)
    {
        CHECK(plan.targets[index] && plan.before[index] != plan.after[index]);
        for (usize prior = 0; prior < index; ++prior)
            CHECK(plan.targets[prior] != plan.targets[index]);
    }
    CHECK(plan.targets[plan.count - 1] == 1);
    CHECK(plan.targets[plan.count - 2] == 2);
    const auto inode = std::span<const u8>{plan.after[0]}.subspan(slot(head), 256);
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    CHECK(ext4_checked::inode_checksum(seed, head, inode));
    CHECK(ext4_checked::le32(inode, 100) == generation + 1);
    CHECK(ext4_checked::le32(inode, 160) == 0x3146524fU);
    CHECK(ext4_checked::le16(inode, 0) == 0);
    for (usize offset = 0; offset < 1024; ++offset)
        if (offset < slot(head) || offset >= slot(head) + 256)
            CHECK(plan.after[0][offset] == plan.before[0][offset]);
    CHECK(ext4_checked::le16(plan.before[plan.count - 1], 58) ==
          ext4_checked::le16(plan.after[plan.count - 1], 58));
    return plan;
}

void apply(Fixture &fixture, Tables &tables, const Ext4OrphanReclaimPlan &plan)
{
    for (usize index = 0; index < plan.count; ++index)
    {
        const u32 target = plan.targets[index];
        if (target == 1)
        {
            CHECK(fixture.super == plan.before[index]);
            fixture.super = plan.after[index];
        }
        else if (target == 2)
        {
            CHECK(fixture.descriptors == plan.before[index]);
            fixture.descriptors = plan.after[index];
        }
        else
        {
            bool found = false;
            for (u32 group = 0; group < 4; ++group)
            {
                if (target == value(fixture.descriptors, group * 32 + 4))
                {
                    CHECK(fixture.inode_bitmaps[group] == plan.before[index]);
                    fixture.inode_bitmaps[group] = plan.after[index]; found = true;
                }
                if (target == value(fixture.descriptors, group * 32))
                {
                    CHECK(fixture.block_bitmaps[group] == plan.before[index]);
                    fixture.block_bitmaps[group] = plan.after[index]; found = true;
                }
                const u32 first = value(fixture.descriptors, group * 32 + 8);
                if (!tables.groups[group].empty() && target >= first && target < first + 512)
                {
                    CHECK(tables.groups[group][target - first] == plan.before[index]);
                    tables.groups[group][target - first] = plan.after[index]; found = true;
                }
            }
            CHECK(found);
        }
    }
    CHECK(fixture.count && fixture.images[0].number == plan.inode);
    for (usize index = 1; index < fixture.count; ++index)
        fixture.images[index - 1] = fixture.images[index];
    --fixture.count;
    if (fixture.count)
        fixture.images[0].table = tables.inode_table(fixture.images[0].number);
    CHECK(value(fixture.super, 232) == plan.next_head);
}

void reject(const Ext4RecoveryInfo &binding, const Fixture &fixture,
            const Tables &tables, u32 head, u32 generation,
            StatusCode code, usize &rejected)
{
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanReclaimPlan plan{};
    plan.count = 5;
    const auto status = Ext4OrphanReclaimPlanner::plan(
        binding, tables.source(fixture), scratch, head, generation, plan);
    CHECK(!status && status.code() == code && !plan.count && !plan.inode);
    ++rejected;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream filled_input(argv[1], std::ios::binary);
    CHECK(filled_input);
    const Image filled_image((std::istreambuf_iterator<char>(filled_input)), {});
    CHECK(filled_image.size() == 32 * 1024 * 1024);
    MemoryDisk filled_disk(filled_image);
    Ext4RecoveryBinding filled_admission;
    CHECK(filled_admission.inspect(filled_disk, {0, filled_image.size() / 512}));
    CHECK(filled_admission.info());
    const auto &filled_binding = *filled_admission.info();
    const usize filled_reads = filled_disk.reads;
    Fixture filled(filled_image, filled_binding);
    filled.add(filled_image, 12, 14);
    filled.add(filled_image, 14, 0);
    filled.head(12);
    Tables filled_tables(filled_image, filled);
    auto first = make(filled_binding, filled, filled_tables, 12, 0);
    CHECK(first.next_head == 14 && first.released_blocks == 4);
    CHECK(value(first.after[4], 12) == value(first.before[4], 12) + 4);
    CHECK(value(first.after[4], 16) == value(first.before[4], 16) + 1);
    CHECK(value(first.after[4], 232) == 14);
    CHECK(first.after[1][(12 - 1) / 8] ==
          (first.before[1][(12 - 1) / 8] & u8(~(1U << ((12 - 1) % 8)))));
    for (u32 block = 530; block < 534; ++block)
    {
        const u32 local = block - 1;
        CHECK(!(first.after[2][local / 8] & (1U << (local % 8))));
    }
    apply(filled, filled_tables, first);
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanOwnershipProof ownership{};
    CHECK(Ext4OrphanOwnershipValidator::validate(
        filled_binding, filled_tables.source(filled), scratch, ownership));
    CHECK(ownership.valid && ownership.observed_head == 14 && ownership.orphan_count == 1);
    auto second = make(filled_binding, filled, filled_tables, 14, 0);
    CHECK(second.next_head == 0 && second.released_blocks == 1);
    apply(filled, filled_tables, second);
    Ext4OrphanChainProof empty_chain{};
    CHECK(Ext4OrphanChainValidator::validate(filled_binding, filled.source(), empty_chain));
    CHECK(empty_chain.count() == 0 && value(filled.super, 12) == value(load(filled_image, 1), 12) + 5);

    std::ifstream empty_input(argv[2], std::ios::binary);
    CHECK(empty_input);
    const Image empty_image((std::istreambuf_iterator<char>(empty_input)), {});
    CHECK(empty_image.size() == filled_image.size());
    MemoryDisk empty_disk(empty_image);
    Ext4RecoveryBinding empty_admission;
    CHECK(empty_admission.inspect(empty_disk, {0, empty_image.size() / 512}));
    CHECK(empty_admission.info());
    const auto &empty_binding = *empty_admission.info();
    const usize empty_reads = empty_disk.reads;
    Fixture empty(empty_image, empty_binding);
    empty.add(empty_image, 14, 0);
    empty.head(14);
    Tables empty_tables(empty_image, empty);
    auto empty_plan = make(empty_binding, empty, empty_tables, 14, 0);
    CHECK(empty_plan.next_head == 0 && empty_plan.released_blocks == 0 &&
          value(empty_plan.after[3], 12) == value(empty_plan.before[3], 12));
    apply(empty, empty_tables, empty_plan);
    CHECK(Ext4OrphanChainValidator::validate(empty_binding, empty.source(), empty_chain));

    usize rejected = 0;
    Fixture bad_head(filled_image, filled_binding);
    bad_head.add(filled_image, 12, 14);
    bad_head.add(filled_image, 14, 0);
    bad_head.head(12);
    Tables bad_tables(filled_image, bad_head);
    reject(filled_binding, bad_head, bad_tables, 14, 0, StatusCode::io_error, rejected);
    reject(filled_binding, bad_head, bad_tables, 12, 1, StatusCode::io_error, rejected);
    reject(filled_binding, bad_head, bad_tables, 0, 0, StatusCode::invalid_argument, rejected);

    auto bad_inode_bitmap = bad_head;
    bad_inode_bitmap.inode_bitmaps[0][1] ^= 1;
    reject(filled_binding, bad_inode_bitmap, bad_tables, 12, 0, StatusCode::io_error, rejected);
    auto bad_block_bitmap = bad_head;
    bad_block_bitmap.block_bitmaps[0][(530 - 1) / 8] ^= u8(1U << ((530 - 1) % 8));
    reject(filled_binding, bad_block_bitmap, bad_tables, 12, 0, StatusCode::io_error, rejected);
    auto bad_count = bad_head;
    store16(std::span<u8>{bad_count.descriptors}, 14,
            u16(ext4_checked::le16(bad_count.descriptors, 14) + 1));
    bad_count.stamp_descriptor(0);
    store32(std::span<u8>{bad_count.super}, 16, value(bad_count.super, 16) + 1);
    bad_count.stamp_super();
    reject(filled_binding, bad_count, bad_tables, 12, 0, StatusCode::io_error, rejected);

    auto exhausted = bad_head;
    store32(std::span<u8>{exhausted.images[0].table}, slot(12) + 100, ~u32{0});
    exhausted.images[0].expected_generation = ~u32{0};
    exhausted.stamp_inode(exhausted.images[0].table, 12);
    Tables exhausted_tables(filled_image, exhausted);
    CHECK(Ext4OrphanOwnershipValidator::validate(
        filled_binding, exhausted_tables.source(exhausted), scratch, ownership));
    reject(filled_binding, exhausted, exhausted_tables, 12, ~u32{0}, StatusCode::overflow, rejected);

    // Move only the fourth block of inode 12 into initialized group 1. The
    // complete ownership proof still passes; the five-home planner declines it.
    auto split = bad_head;
    auto split_inode = std::span<u8>{split.images[0].table}.subspan(slot(12), 256);
    store16(split_inode, 42, 2);
    store16(split_inode, 56, 3);
    store32(split_inode, 64, 3);
    store16(split_inode, 68, 1);
    store32(split_inode, 72, 12805);
    split.stamp_inode(split.images[0].table, 12);
    const u32 old_local = 533 - 1;
    split.block_bitmaps[0][old_local / 8] &= u8(~(1U << (old_local % 8)));
    split.stamp_block_bitmap(0);
    const u32 new_local = (12805 - 1) % 8192;
    CHECK(!(split.block_bitmaps[1][new_local / 8] & (1U << (new_local % 8))));
    split.block_bitmaps[1][new_local / 8] |= u8(1U << (new_local % 8));
    split.stamp_block_bitmap(1);
    store16(std::span<u8>{split.descriptors}, 12,
            u16(ext4_checked::le16(split.descriptors, 12) + 1));
    store16(std::span<u8>{split.descriptors}, 32 + 12,
            u16(ext4_checked::le16(split.descriptors, 32 + 12) - 1));
    split.stamp_descriptor(0); split.stamp_descriptor(1);
    Tables split_tables(filled_image, split);
    CHECK(Ext4OrphanOwnershipValidator::validate(
        filled_binding, split_tables.source(split), scratch, ownership));
    reject(filled_binding, split, split_tables, 12, 0, StatusCode::unsupported, rejected);

    CHECK(rejected == 8 && filled_disk.reads == filled_reads && empty_disk.reads == empty_reads &&
          !filled_disk.writes && !filled_disk.flushes &&
          !empty_disk.writes && !empty_disk.flushes);
    std::printf("PASS orphan reclaim plan: filled-chain=2 empty=1 rejects=%zu no_device_io=1\n", rejected);
}
