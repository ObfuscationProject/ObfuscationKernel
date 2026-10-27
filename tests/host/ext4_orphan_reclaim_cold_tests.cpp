#include "ok/fs/ext4/alloc.hpp"
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

bool legal(usize count, unsigned mask)
{
    const unsigned all = (1U << count) - 1;
    if (!mask) return true;
    if (!(mask & 1)) return false;
    return !(mask & (1U << (count - 1))) || mask == all;
}

usize accepted{}, barriers{}, rejected{}, alternate_history{};
void verify(const Scenario &scenario, const Ext4OrphanReclaimJournalPlan &records,
    const Observed &observed, Ext4OrphanReclaimColdScratch &scratch, bool success, const char *label)
{
    Ext4OrphanReclaimColdResult result{}; result.valid = true; result.inode = 55;
    const auto status = Ext4OrphanReclaimColdVerifier::verify(scenario.active(records), scenario.records(records), observed.source(), scratch, result);
    if (bool(status) != success)
    {
        std::fprintf(stderr, "FAIL %s: %u %.*s\n", label, unsigned(status.code()), int(status.message().size()), status.message().data());
        std::abort();
    }
    CHECK(result.valid == success && (success || (!result.inode && !result.count)));
}

void mixtures(Scenario &scenario, Ext4OrphanReclaimColdScratch &scratch)
{
    const usize count = scenario.reclaim.reclaim.count;
    for (unsigned mask = 0; mask < (1U << count); ++mask)
    {
        const auto image = scenario.mixture(mask); Observed observed(image, scenario.old_binding);
        Ext4OrphanReclaimColdResult result{}; result.valid = true;
        const auto status = Ext4OrphanReclaimColdVerifier::verify(scenario.active(scenario.journal), scenario.records(scenario.journal), observed.source(), scratch, result);
        const bool expected = legal(count, mask);
        if (bool(status) != expected)
        {
            std::fprintf(stderr, "FAIL count=%zu mask=%u: %u %.*s\n", count, mask, unsigned(status.code()), int(status.message().size()), status.message().data());
            std::abort();
        }
        if (!expected) { CHECK(!result.valid && !result.count); ++barriers; continue; }
        CHECK(result.valid && result.complete_global_before_proof && result.count == count);
        CHECK(result.inode == scenario.reclaim.reclaim.inode && result.next_head == scenario.reclaim.reclaim.next_head);
        CHECK(result.active_sequence == scenario.journal.sequence && result.parsed_next_sequence == result.active_sequence + 1 && result.cleanup_sequence == result.active_sequence + 2);
        CHECK(result.all_before == (mask == 0) && result.all_after == (mask == (1U << count) - 1));
        for (usize index = 0; index < count; ++index)
            CHECK(result.home_after[index] == bool(mask & (1U << index)) && result.targets[index] == scenario.reclaim.reclaim.targets[index]);
        ++accepted;
    }
}

void hostile(Scenario &scenario, Ext4OrphanReclaimColdScratch &scratch)
{
    const unsigned all = (1U << scenario.journal.count) - 1;
    const auto final = scenario.mixture(all); Observed after(final, scenario.old_binding);
    const auto original = scenario.journal;
    {
        auto binding = scenario.active(original); binding.layout.first_sector = 17;
        Ext4OrphanReclaimColdResult result{}; result.valid = true; result.inode = 55;
        CHECK(!Ext4OrphanReclaimColdVerifier::verify(binding, scenario.records(original), after.source(), scratch, result));
        CHECK(!result.valid && !result.inode && !result.count); ++rejected;
    }
    auto reject_records = [&](Ext4OrphanReclaimJournalPlan plan, const char *label) {
        verify(scenario, plan, after, scratch, false, label); ++rejected;
    };
    auto broken = original; broken.commit[16] ^= 1; reject_records(broken, "commit CRC");
    broken = original; broken.descriptor[1020] ^= 1; reject_records(broken, "descriptor CRC");
    broken = original; broken.payloads[0][900] ^= 1; reject_records(broken, "payload CRC");
    broken = original; broken.end[0] = 1; reject_records(broken, "noncanonical end");
    broken = original; broken.count = broken.count == 4 ? 5 : 4; reject_records(broken, "participant count");
    {
        auto observed = after;
        const auto target = scenario.reclaim.reclaim.targets[0];
        const u32 group = (scenario.reclaim.reclaim.inode - 1) / 2048;
        const u32 first = value(observed.fixture.descriptors, group * 32 + 8);
        observed.tables.groups[group][target - first][900] ^= 1;
        verify(scenario, original, observed, scratch, false, "foreign whole home sibling"); ++rejected;
    }
    {
        auto observed = after; observed.directories.blocks.pop_back();
        verify(scenario, original, observed, scratch, false, "missing directory"); ++rejected;
    }
    {
        auto observed = after; observed.directories.blocks.push_back(observed.directories.blocks[0]);
        verify(scenario, original, observed, scratch, false, "duplicate directory"); ++rejected;
    }
    {
        auto observed = after; observed.directories.blocks[0].bytes[200] ^= 1;
        verify(scenario, original, observed, scratch, false, "directory checksum"); ++rejected;
    }
    {
        auto observed = after; observed.tables.groups[0].pop_back();
        verify(scenario, original, observed, scratch, false, "incomplete table"); ++rejected;
    }
    {
        auto observed = after;
        auto &table = observed.tables.groups[0][1];
        store16(table, slot(8), 0100644); observed.fixture.stamp_inode(table, 8);
        verify(scenario, original, observed, scratch, false, "strict journal inode mode"); ++rejected;
    }
    {
        auto observed = after;
        observed.tables.groups[0][0][slot(2) + 8] ^= 1;
        verify(scenario, original, observed, scratch, false, "unrelated allocated inode checksum"); ++rejected;
    }
    {
        auto observed = after;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, scenario.old_binding.uuid);
        const u32 root_table = value(observed.fixture.descriptors, 8);
        const auto root = std::span<const u8>{observed.tables.groups[0][0]}.subspan(slot(2), 256);
        (void)root_table;
        bool inserted = false;
        for (auto &directory : observed.directories.blocks)
            if (directory.inode == 2)
                for (usize at = 0; at < 1012; at += ext4_checked::le16(directory.bytes, at + 4))
                    if (!ext4_checked::le32(directory.bytes, at) && directory.bytes[at + 6])
                    {
                        store32(directory.bytes, at, scenario.reclaim.reclaim.inode);
                        store32(directory.bytes, 1020, jbd2::detail::crc32c(ext4_checked::inode_seed(seed, 2, root), std::span<const u8>{directory.bytes}.first(1012)));
                        inserted = true; break;
                    }
        CHECK(inserted);
        verify(scenario, original, observed, scratch, false, "live name references declared orphan"); ++rejected;
    }
    // A coherent alternate overwritten ctime is a declared history, not an
    // authenticated old device image. All-before rejects; all-after may admit.
    auto alternative = scenario.reclaim.reclaim;
    const u32 inode = alternative.inode;
    const usize offset = slot(inode);
    store32(alternative.before[0], offset + 12, value(alternative.before[0], offset + 12) + 1);
    Fixture stamp(scenario.before, scenario.old_binding); stamp.stamp_inode(alternative.before[0], inode);
    Ext4OrphanReclaimJournalPlan foreign{};
    Ext4OrphanReclaimJournalScratch formatter{};
    CHECK(Ext4OrphanReclaimJournalPlanner::plan(scenario.old_binding, alternative,
        load(scenario.before, u32(scenario.old_binding.layout.journal_first_block)), 7, 13, formatter, foreign));
    Observed before(scenario.before, scenario.old_binding);
    verify(scenario, foreign, before, scratch, false, "alternate history with before home"); ++rejected;
    verify(scenario, foreign, after, scratch, true, "self-consistent alternate overwritten history"); ++alternate_history;
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    auto scratch = std::make_unique<Ext4OrphanReclaimColdScratch>();
    Scenario filled(read_image(argv[1]), true); mixtures(filled, *scratch); hostile(filled, *scratch);
    // The next orphan is planned from actual first-head final bytes and the
    // reserved clean sequence; no caller-prepared chain is trusted by verifier.
    for (usize index = 0; index < filled.reclaim.reclaim.count; ++index)
        apply(filled.before, filled.reclaim.reclaim.targets[index], filled.reclaim.reclaim.after[index]);
    auto clean = filled.journal.journal_head;
    const u32 next_sequence = filled.journal.sequence + 2;
    for (usize at = 0; at < 4; ++at) clean[24 + at] = u8(next_sequence >> (24 - at * 8));
    for (usize at = 28; at < 32; ++at) clean[at] = 0;
    for (usize at = 252; at < 256; ++at) clean[at] = 0;
    const u32 crc = jbd2::detail::crc32c(~u32{0}, clean);
    for (usize at = 0; at < 4; ++at) clean[252 + at] = u8(crc >> (24 - at * 8));
    apply(filled.before, u32(filled.old_binding.layout.journal_first_block), clean);
    filled.old_binding.journal_sequence = next_sequence;
    filled.prepare(14); mixtures(filled, *scratch); hostile(filled, *scratch);
    Scenario empty(read_image(argv[2]), false); mixtures(empty, *scratch); hostile(empty, *scratch);
    std::printf("PASS pure semantic reclaim cold: accepted=%zu barrier_rejects=%zu hostile_rejects=%zu declared_history_accepts=%zu external_write_flush=0\n", accepted, barriers, rejected, alternate_history);
}
