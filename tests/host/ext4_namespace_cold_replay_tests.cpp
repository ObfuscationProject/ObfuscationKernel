#include "ext4_namespace_journal_fixture.hpp"
#include <ok/fs/ext4/recovery.hpp>

using namespace namespace_journal_model;
namespace
{
usize mixtures{}, rejected{}, escaped{}, capacity_rejected{};
void big32(Block &block, usize at, u32 number)
{ for (usize i = 0; i < 4; ++i) block[at + i] = u8(number >> (24 - i * 8)); }
void dirty(Fixture &fixture)
{
    store32(fixture.super, 96, value(fixture.super, 96) | 4);
    store32(fixture.super, 1020, jbd2::detail::crc32c(~u32{0},
        std::span<const u8>{fixture.super}.first(1020)));
}
void empty_regular(Fixture &fixture, u32 number)
{
    auto &table = number == fixture.source_number ? fixture.source : fixture.destination;
    auto inode = std::span<u8>{table}.subspan(slot(number), 256);
    CHECK(ext4_checked::le16(inode, 42) == 1 && ext4_checked::le16(inode, 56) == 1);
    const u32 physical = ext4_checked::le32(inode, 60), local = physical - 1;
    CHECK(physical > 0 && physical < 8193 &&
          (fixture.block_bitmaps[0][local / 8] & (1U << (local % 8))));
    fixture.block_bitmaps[0][local / 8] &= u8(~(1U << (local % 8)));
    store16(fixture.descriptors, 12, u16(short_value(fixture.descriptors, 12) + 1));
    store32(fixture.super, 12, value(fixture.super, 12) + 1);
    store32(fixture.super, 1020, jbd2::detail::crc32c(~u32{0},
        std::span<const u8>{fixture.super}.first(1020)));
    fixture.stamp_block_bitmap(0);
    store32(table, slot(number) + 4, 0); store32(table, slot(number) + 28, 0);
    store16(table, slot(number) + 42, 0);
    for (usize at = slot(number) + 52; at < slot(number) + 100; ++at) table[at] = 0;
    fixture.stamp_inode(table, number);
    if (fixture.source_block == fixture.destination_block)
    { fixture.source = table; fixture.destination = table; }
}
Ext4NamespaceReplayRecords records(const Ext4NamespaceJournalPlan &journal)
{
    return {journal.journal_head, journal.descriptor,
        std::span<const Block>{journal.payloads}.first(journal.count), journal.commit, journal.end};
}
void stamp_descriptor(Ext4NamespaceJournalPlan &journal, const Ext4RecoveryInfo &binding)
{
    big32(journal.descriptor, 1020, 0);
    big32(journal.descriptor, 1020,
        jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), journal.descriptor));
}
void stamp_payload(Ext4NamespaceJournalPlan &journal, const Ext4RecoveryInfo &binding, usize home)
{
    const std::array<u8, 4> sequence{u8(journal.sequence >> 24), u8(journal.sequence >> 16),
        u8(journal.sequence >> 8), u8(journal.sequence)};
    const usize at = home == 0 ? 12 : 44 + (home - 1) * 16;
    big32(journal.descriptor, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(
        jbd2::detail::crc32c(~u32{0}, binding.uuid), sequence), journal.payloads[home]));
    stamp_descriptor(journal, binding);
}
void expect_rejected(const Ext4RecoveryInfo &binding, const Fixture &fixture,
    const Block &super, const Ext4NamespaceJournalPlan &journal, std::span<const u32> targets,
    std::span<const Block> homes)
{
    Ext4NamespaceColdScratch scratch{};
    Ext4NamespaceColdResult result{};
    result.durable_old_witness = true; result.homes.count = 5;
    const auto status = Ext4NamespaceColdReplayVerifier::verify(binding, records(journal),
        {super, fixture.descriptors, fixture.inode_bitmaps, fixture.block_bitmaps},
        {targets, homes}, scratch, result);
    CHECK(!status && !result.durable_old_witness && !result.homes.count);
    ++rejected;
}
void exercise(const Ext4RecoveryInfo &old_binding, const Fixture &fixture,
    const Ext4NamespaceUpdate &update, const Block &old_head, usize expected_count)
{
    Ext4NamespaceJournalScratch prepared{};
    Ext4NamespaceJournalPlan journal{};
    CHECK(Ext4NamespaceDurableJournalPlanner::plan(old_binding, update, fixture.images(),
        old_head, prepared, journal));
    CHECK(journal.ready && !journal.no_op && journal.count == expected_count &&
          Ext4NamespaceWitnessCodec::present(journal.descriptor));
    const auto &canonical = prepared.namespace_plan;
    Ext4RecoveryInfo binding = old_binding;
    binding.empty_log = false; binding.journal_sequence = journal.sequence;
    const auto targets = std::span<const u32>{journal.targets}.first(journal.count);
    std::array<Block, 5> homes{};
    const auto images = [&] { return std::span<const Block>{homes}.first(journal.count); };
    Block super = fixture.super;
    for (u32 mask = 0; mask < (1U << journal.count); ++mask)
    {
        for (usize i = 0; i < journal.count; ++i)
        {
            homes[i] = mask & (1U << i) ? canonical.after[i] : canonical.before[i];
            if (journal.targets[i] == 1) super = homes[i];
        }
        Ext4NamespaceColdScratch scratch{};
        Ext4NamespaceColdResult result{};
        const auto status = Ext4NamespaceColdReplayVerifier::verify(binding, records(journal),
            {super, fixture.descriptors, fixture.inode_bitmaps, fixture.block_bitmaps},
            {targets, images()}, scratch, result);
        if (!status) std::fprintf(stderr, "cold status: %.*s\n",
            int(status.message().size()), status.message().data());
        CHECK(status && result.durable_old_witness && !result.homes.conditional_on_old_witness &&
            result.homes.count == journal.count && result.homes.allocation_unchanged &&
            result.homes.orphan_published == canonical.orphan_published &&
            result.operation == update.operation && result.source == update.source &&
            result.removed_inode == canonical.removed_inode && result.witness_runs > 0);
        CHECK(result.homes.new_homes == usize(__builtin_popcount(mask)) &&
            result.homes.old_homes + result.homes.new_homes == journal.count &&
            result.homes.all_before == (mask == 0) &&
            result.homes.all_after == (mask == (1U << journal.count) - 1));
        for (usize i = 0; i < journal.count; ++i)
            CHECK(scratch.before[i] == canonical.before[i] && scratch.after[i] == canonical.after[i]);
        ++mixtures;
    }
    if (jbd2::detail::be32(canonical.after[0], 0) == jbd2::magic)
    { CHECK(jbd2::detail::be32(journal.payloads[0], 0) == 0); ++escaped; }
    const auto reject = [&](const Ext4NamespaceJournalPlan &foreign) {
        expect_rejected(binding, fixture, super, foreign, targets, images());
    };
    homes[0][200] ^= 1; reject(journal); homes[0] = canonical.after[0];
    auto foreign = journal;
    foreign.descriptor[128] ^= 1; stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store16(foreign.descriptor, 136, 2);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[140] = 6;
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[141] = 255;
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store16(foreign.descriptor, 138, 1021);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[178] = 1;
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[231] = 1;
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[1019] = 1;
    CHECK(short_value(journal.descriptor, 138) < 1019);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store32(foreign.descriptor, 160, value(foreign.descriptor, 160) + 1);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[180] ^= 1;
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.descriptor[264] = u8(journal.count);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store16(foreign.descriptor, 265, 1024);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store16(foreign.descriptor, 267, 0);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; store16(foreign.descriptor, 267, 65535);
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal;
    foreign.descriptor[269] = canonical.after[journal.descriptor[264]][short_value(journal.descriptor, 265)];
    stamp_descriptor(foreign, binding); reject(foreign);
    foreign = journal; foreign.payloads[0][200] ^= 1;
    stamp_payload(foreign, binding, 0); reject(foreign);
    foreign = journal; big32(foreign.commit, 52, update.seconds + 1);
    big32(foreign.commit, 16, 0);
    big32(foreign.commit, 16,
        jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), foreign.commit));
    reject(foreign);
    foreign = journal; foreign.end[200] = 1; reject(foreign);
    foreign = journal; foreign.descriptor[264] ^= 1; reject(foreign); // Incorrect JBD2 CRC.
    auto unordered = journal.targets; std::swap(unordered[0], unordered[1]);
    expect_rejected(binding, fixture, super, journal,
        std::span<const u32>{unordered}.first(journal.count), images());
    if (canonical.orphan_published)
    {
        Block wrong_super = super; wrong_super[232] ^= 1;
        expect_rejected(binding, fixture, wrong_super, journal, targets, images());
    }
    Ext4NamespaceJournalScratch legacy_scratch{};
    Ext4NamespaceJournalPlan legacy{};
    CHECK(Ext4NamespaceJournalPlanner::plan(old_binding, update, fixture.images(),
        old_head, legacy_scratch, legacy));
    reject(legacy); // No guessed history for witness-free redo.
}
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream filled_input(argv[1], std::ios::binary); CHECK(filled_input);
    const Image filled((std::istreambuf_iterator<char>(filled_input)), {});
    std::ifstream empty_input(argv[2], std::ios::binary); CHECK(empty_input);
    const Image empty((std::istreambuf_iterator<char>(empty_input)), {});
    CHECK(filled.size() == 32 * 1024 * 1024 && empty.size() == filled.size());
    MemoryDisk filled_disk(filled), empty_disk(empty);
    Ext4RecoveryBinding filled_admission, empty_admission;
    CHECK(filled_admission.inspect(filled_disk, {0, filled.size() / 512}));
    CHECK(empty_admission.inspect(empty_disk, {0, empty.size() / 512}));
    auto filled_binding = *filled_admission.info(), empty_binding = *empty_admission.info();
    filled_binding.needs_recovery = empty_binding.needs_recovery = true;
    filled_binding.empty_log = empty_binding.empty_log = true;
    const Block filled_head = load(filled, u32(filled_binding.layout.journal_first_block));
    const Block empty_head = load(empty, u32(empty_binding.layout.journal_first_block));
    const usize filled_reads = filled_disk.reads, empty_reads = empty_disk.reads;
    Fixture base(filled, filled_binding); dirty(base);
    Fixture empty_file(empty, empty_binding); dirty(empty_file);
    exercise(filled_binding, base, base.replacement("basic-note", "second-note"), filled_head, 4);
    exercise(filled_binding, base, base.unlink("basic-note"), filled_head, 4);
    exercise(empty_binding, empty_file, empty_file.unlink("empty-note"), empty_head, 4);
    Fixture empty_source(filled, filled_binding); dirty(empty_source); empty_regular(empty_source, 14);
    exercise(filled_binding, empty_source, empty_source.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture empty_victim(filled, filled_binding); dirty(empty_victim); empty_regular(empty_victim, 15);
    exercise(filled_binding, empty_victim, empty_victim.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture both_empty(filled, filled_binding); dirty(both_empty);
    empty_regular(both_empty, 14); empty_regular(both_empty, 15);
    exercise(filled_binding, both_empty, both_empty.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture nonlast(filled, filled_binding); dirty(nonlast);
    nonlast.destination = nonlast.source; nonlast.destination_block = nonlast.source_block;
    store16(nonlast.source, slot(14) + 26, 2); nonlast.stamp_inode(nonlast.source, 14);
    nonlast.destination = nonlast.source; nonlast.add_alias(14, "second-note", "basic-link");
    exercise(filled_binding, nonlast, nonlast.unlink("basic-note"), filled_head, 3);
    Fixture replaced_nonlast(filled, filled_binding); dirty(replaced_nonlast);
    store16(replaced_nonlast.destination, slot(15) + 26, 2);
    replaced_nonlast.stamp_inode(replaced_nonlast.destination, 15);
    replaced_nonlast.source = replaced_nonlast.destination;
    replaced_nonlast.add_alias(15, "second-note", "second-link");
    exercise(filled_binding, replaced_nonlast,
        replaced_nonlast.replacement("basic-note", "second-note"), filled_head, 3);
    Fixture wide(filled, filled_binding, 12, 14); dirty(wide);
    exercise(filled_binding, wide, wide.replacement("seed", "basic-note"), filled_head, 5);
    Fixture old_stamps(filled, filled_binding); dirty(old_stamps);
    for (const u32 inode : {14U, 15U})
    {
        auto &table = old_stamps.source;
        store32(table, slot(inode) + 36, 0xffffffffU); store32(table, slot(inode) + 152, 3);
        store32(table, slot(inode) + 12, 1700000111); store32(table, slot(inode) + 16, 1700000222);
        store32(table, slot(inode) + 100, 0xabbadd00U + inode);
        old_stamps.stamp_inode(table, inode);
    }
    old_stamps.destination = old_stamps.source;
    exercise(filled_binding, old_stamps,
        old_stamps.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture escaped_table(filled, filled_binding); dirty(escaped_table);
    big32(escaped_table.parent, 0, jbd2::magic);
    exercise(filled_binding, escaped_table,
        escaped_table.replacement("basic-note", "second-note"), filled_head, 4);

    // A legal large record can require more undo bytes than the fixed witness.
    Fixture oversized(filled, filled_binding, 15, 14); dirty(oversized);
    const usize at = oversized.entry("second-note"); CHECK(at < 1012);
    const usize record = short_value(oversized.directory, at + 4);
    for (usize i = at + 8 + oversized.directory[at + 6]; i < at + record; ++i)
        oversized.directory[i] = 0xa5;
    oversized.stamp_directory();
    Ext4NamespaceJournalScratch preparation{};
    Ext4NamespaceJournalPlan legacy{}, durable{};
    CHECK(Ext4NamespaceJournalPlanner::plan(filled_binding, oversized.unlink("second-note"),
        oversized.images(), filled_head, preparation, legacy));
    CHECK(!Ext4NamespaceDurableJournalPlanner::plan(filled_binding, oversized.unlink("second-note"),
        oversized.images(), filled_head, preparation, durable) && !durable.ready && !durable.count);
    ++capacity_rejected;
    CHECK(filled_disk.reads == filled_reads && empty_disk.reads == empty_reads &&
          !filled_disk.writes && !filled_disk.flushes && !empty_disk.writes && !empty_disk.flushes);
    std::printf("PASS durable namespace cold replay mixtures=%zu rejected=%zu escaped=%zu capacity_rejected=%zu no_device_io=1 mounted_authority=0\n",
        mixtures, rejected, escaped, capacity_rejected);
}
