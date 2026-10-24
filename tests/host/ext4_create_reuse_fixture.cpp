#include "ok/fs/ext4_orphan_reclaim_plan.hpp"
#include "ok/fs/ext4_create_plan.hpp"
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

} // namespace

// This setup applies a pure reclaim plan to caller-owned bytes. It is not an
// executed reclaim writer, reference retirement, or directory validation proof.
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Image image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    MemoryDisk disk(image); Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, image.size() / 512}));
    const auto &binding = *admission.info();
    Fixture fixture(image, binding);
    fixture.add(image, 12, 14); fixture.add(image, 14, 0); fixture.head(12);
    Tables tables(image, fixture);
    apply(fixture, tables, make(binding, fixture, tables, 12, 0));
    apply(fixture, tables, make(binding, fixture, tables, 14, 0));
    const auto write = [&](u32 number, const Block &block) {
        for (usize i = 0; i < 1024; ++i) image[usize(number) * 1024 + i] = block[i];
    };
    for (u32 group = 0; group < 4; ++group)
    {
        write(value(fixture.descriptors, group * 32), fixture.block_bitmaps[group]);
        write(value(fixture.descriptors, group * 32 + 4), fixture.inode_bitmaps[group]);
        const u32 first = value(fixture.descriptors, group * 32 + 8);
        for (usize i = 0; i < tables.groups[group].size(); ++i) write(first + u32(i), tables.groups[group][i]);
    }
    const auto parent = std::span<const u8>{tables.inode_table(2)}.subspan(slot(2), 256);
    const u32 directory_block = ext4_checked::le32(parent, 60);
    auto directory = load(image, directory_block);
    // Remove the two former names from the synthetic reclaim fixture.
    store32(directory, 44, 0); store32(directory, 124, 0);
    store32(directory, 1020, jbd2::detail::crc32c(ext4_checked::inode_seed(
        jbd2::detail::crc32c(~u32{0}, binding.uuid), 2, parent), std::span<const u8>{directory}.first(1012)));
    write(directory_block, directory);
    store32(fixture.super, 96, value(fixture.super, 96) | 4U); fixture.stamp_super();
    write(1, fixture.super); write(2, fixture.descriptors);
    std::ofstream output(argv[2], std::ios::binary); CHECK(output);
    output.write(reinterpret_cast<const char *>(image.data()), image.size()); CHECK(output);
    CHECK(!disk.writes && !disk.flushes);
    std::puts("PASS synthetic reclaimed fixture: inode12 generation1, inode14 generation1, no reclaim device I/O");
}
