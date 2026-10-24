#include "ok/fs/ext4_orphan_reclaim_cold.hpp"
#include "snapshot_fixture.hpp"
#include <memory>

namespace
{
void apply(Image &image, u32 physical, const Block &bytes)
{ for (usize at = 0; at < 1024; ++at) image[usize{physical} * 1024 + at] = bytes[at]; }

struct Observed
{
    Fixture fixture;
    Tables tables;
    DirectoryImages directories;
    Observed(const Image &image, const Ext4RecoveryInfo &binding)
        : fixture(image, binding), tables(image, fixture), directories(image, fixture, tables) {}
    Ext4OrphanReclaimColdObserved source() const
    {
        std::array<std::span<const Block>, 4> groups{};
        for (usize group = 0; group < 4; ++group) groups[group] = tables.groups[group];
        return {fixture.super, fixture.descriptors, fixture.inode_bitmaps, fixture.block_bitmaps, groups, directories.blocks};
    }
};

struct Scenario
{
    Ext4RecoveryInfo old_binding{};
    Image before;
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> qualified =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim{};
    Ext4OrphanReclaimJournalScratch formatter{};
    Ext4OrphanReclaimJournalPlan journal{};

    Scenario(const Image &original, bool populated) : before(original)
    {
        MemoryDisk disk(original); Ext4RecoveryBinding admission;
        CHECK(admission.inspect(disk, {0, original.size() / 512})); CHECK(admission.info());
        old_binding = *admission.info(); old_binding.needs_recovery = true;
        Fixture fixture(original, old_binding);
        if (populated) fixture.add(original, 12, 14);
        fixture.add(original, 14, 0); fixture.head(populated ? 12 : 14);
        store32(fixture.super, 96, 0x46); fixture.stamp_super();
        Tables tables(original, fixture); DirectoryImages directories(original, fixture, tables);
        if (populated) directories.remove_target(fixture, tables, 12);
        directories.remove_target(fixture, tables, 14);
        apply(before, 1, fixture.super);
        for (usize at = 0; at < fixture.count; ++at) apply(before, fixture.images[at].table_block, fixture.images[at].table);
        for (const auto &directory : directories.blocks) apply(before, directory.physical, directory.bytes);
        prepare(populated ? 12 : 14);
    }
    void prepare(u32 inode)
    {
        Fixture fixture(before, old_binding);
        if (inode == 12) { fixture.add(before, 12, 14); fixture.add(before, 14, 0); }
        else fixture.add(before, 14, 0);
        // add() increments namespace i_version; use exact current tables instead.
        for (usize at = 0; at < fixture.count; ++at)
        { fixture.images[at].table = load(before, fixture.images[at].table_block); }
        Tables tables(before, fixture); DirectoryImages directories(before, fixture, tables);
        Ext4OrphanDirectoryReferenceSource source{tables.source(fixture), directories.blocks};
        CHECK(Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(old_binding, source, *qualified, inode,
            ext4_checked::le32(fixture.images[0].table, slot(inode) + 100), reclaim));
        CHECK(reclaim.valid);
        CHECK(Ext4OrphanReclaimJournalPlanner::plan(old_binding, reclaim.reclaim,
            load(before, u32(old_binding.layout.journal_first_block)), 7, 13, formatter, journal));
    }
    Ext4RecoveryInfo active(const Ext4OrphanReclaimJournalPlan &records) const
    { auto binding = old_binding; binding.empty_log = false; binding.journal_sequence = records.sequence; return binding; }
    Image mixture(unsigned mask) const
    {
        auto image = before;
        for (usize index = 0; index < reclaim.reclaim.count; ++index)
            if (mask & (1U << index)) apply(image, reclaim.reclaim.targets[index], reclaim.reclaim.after[index]);
        return image;
    }
    Ext4OrphanReclaimColdRecords records(const Ext4OrphanReclaimJournalPlan &plan) const
    { return {plan.journal_head, plan.descriptor, plan.commit, plan.end, std::span<const Block>{plan.payloads}.first(plan.count)}; }
};

} // namespace
