#include "ext4_namespace_journal_fixture.hpp"
#include <ok/fs/ext4/recovery.hpp>

using namespace namespace_journal_model;
namespace
{
usize mixtures{}, rejected{};

void big32(Block &block, usize at, u32 number)
{ for (usize index = 0; index < 4; ++index) block[at + index] = u8(number >> (24 - index * 8)); }
void dirty(Fixture &fixture)
{
    store32(fixture.super, 96, value(fixture.super, 96) | 4);
    store32(fixture.super, 1020,
        jbd2::detail::crc32c(~u32{0}, std::span<const u8>{fixture.super}.first(1020)));
}
void checked_empty_regular(Fixture &fixture, u32 number)
{
    CHECK(number == fixture.source_number || number == fixture.destination_number);
    auto &table = number == fixture.source_number ? fixture.source : fixture.destination;
    auto inode = std::span<u8>{table}.subspan(slot(number), 256);
    CHECK(ext4_checked::le16(inode, 42) == 1 && ext4_checked::le16(inode, 56) == 1);
    const u32 physical = ext4_checked::le32(inode, 60);
    CHECK(physical > 0 && physical < 8193);
    const u32 local = physical - 1;
    CHECK(fixture.block_bitmaps[0][local / 8] & (1U << (local % 8)));
    fixture.block_bitmaps[0][local / 8] &= u8(~(1U << (local % 8)));
    store16(fixture.descriptors, 12, u16(short_value(fixture.descriptors, 12) + 1));
    store32(fixture.super, 12, value(fixture.super, 12) + 1);
    store32(fixture.super, 1020,
        jbd2::detail::crc32c(~u32{0}, std::span<const u8>{fixture.super}.first(1020)));
    fixture.stamp_block_bitmap(0);
    store32(table, slot(number) + 4, 0);
    store32(table, slot(number) + 28, 0);
    store16(table, slot(number) + 42, 0);
    for (usize at = slot(number) + 52; at < slot(number) + 100; ++at) table[at] = 0;
    fixture.stamp_inode(table, number);
    if (fixture.source_block == fixture.destination_block)
    { fixture.source = table; fixture.destination = table; }
}
Ext4NamespaceReplayRecords records(const Ext4NamespaceJournalPlan &journal)
{
    return {journal.journal_head, journal.descriptor,
            std::span<const Block>{journal.payloads}.first(journal.count),
            journal.commit, journal.end};
}
void stamp_descriptor(Ext4NamespaceJournalPlan &journal, const Ext4RecoveryInfo &binding)
{
    big32(journal.descriptor, 1020, 0);
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    big32(journal.descriptor, 1020, jbd2::detail::crc32c(seed, journal.descriptor));
}
void stamp_payload_tag(Ext4NamespaceJournalPlan &journal, const Ext4RecoveryInfo &binding, usize index)
{
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    const std::array<u8, 4> sequence{u8(journal.sequence >> 24), u8(journal.sequence >> 16),
                                      u8(journal.sequence >> 8), u8(journal.sequence)};
    const usize at = index == 0 ? 12 : 44 + (index - 1) * 16;
    big32(journal.descriptor, at + 12,
          jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), journal.payloads[index]));
    stamp_descriptor(journal, binding);
}
void stamp_commit(Ext4NamespaceJournalPlan &journal, const Ext4RecoveryInfo &binding)
{
    big32(journal.commit, 16, 0);
    big32(journal.commit, 16,
          jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), journal.commit));
}
void stamp_inode(Block &table, u32 number, const Ext4RecoveryInfo &binding)
{
    auto inode = std::span<u8>{table}.subspan(slot(number), 256);
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
    store16(table, slot(number) + 124, u16(checksum));
    store16(table, slot(number) + 130, u16(checksum >> 16));
}
void expect_rejected(const Ext4RecoveryInfo &binding, const Ext4NamespaceUpdate &update,
                     const Fixture &fixture, const Block &old_head,
                     const Ext4NamespaceJournalPlan &journal, std::span<const u32> targets,
                     std::span<const Block> images)
{
    Ext4NamespaceReplayScratch scratch{};
    Ext4NamespaceReplayResult result{};
    result.count = 5; result.conditional_on_old_witness = true;
    const auto status = Ext4NamespaceReplayVerifier::verify(binding, update, fixture.images(),
        old_head, records(journal), {targets, images}, scratch, result);
    CHECK(!status && !result.count && !result.conditional_on_old_witness);
    ++rejected;
}
void exercise(const Ext4RecoveryInfo &binding, const Fixture &fixture,
              Ext4NamespaceUpdate update, const Block &old_head, usize expected_count)
{
    Ext4NamespaceJournalScratch preparation{};
    Ext4NamespaceJournalPlan journal{};
    CHECK(Ext4NamespaceJournalPlanner::plan(binding, update, fixture.images(), old_head,
                                           preparation, journal));
    const auto &canonical = preparation.namespace_plan;
    CHECK(journal.ready && !journal.no_op && journal.count == expected_count &&
          canonical.count == expected_count && canonical.allocation_unchanged);
    std::array<u32, 5> targets = journal.targets;
    std::array<Block, 5> images{};
    const auto target_span = std::span<const u32>{targets}.first(journal.count);
    const auto image_span = [&] { return std::span<const Block>{images}.first(journal.count); };
    for (u32 mask = 0; mask < (1U << journal.count); ++mask)
    {
        for (usize index = 0; index < journal.count; ++index)
            images[index] = mask & (1U << index) ? canonical.after[index] : canonical.before[index];
        Ext4NamespaceReplayScratch scratch{};
        Ext4NamespaceReplayResult result{};
        const auto status = Ext4NamespaceReplayVerifier::verify(binding, update, fixture.images(),
            old_head, records(journal), {target_span, image_span()}, scratch, result);
        if (!status)
            std::fprintf(stderr, "namespace replay status: %.*s\n",
                         int(status.message().size()), status.message().data());
        CHECK(status && result.count == journal.count && result.conditional_on_old_witness &&
              result.allocation_unchanged && result.orphan_published == canonical.orphan_published);
        CHECK(result.new_homes == usize(__builtin_popcount(mask)) &&
              result.old_homes + result.new_homes == journal.count &&
              result.all_before == (mask == 0) &&
              result.all_after == (mask == (1U << journal.count) - 1));
        for (usize index = 0; index < journal.count; ++index)
            CHECK(result.targets[index] == journal.targets[index] &&
                  result.home_after[index] == bool(mask & (1U << index)));
        ++mixtures;
    }
    for (usize index = 0; index < journal.count; ++index) images[index] = canonical.after[index];
    images[0][200] ^= 1; // An unrelated byte in a participant table block.
    expect_rejected(binding, update, fixture, old_head, journal, target_span, image_span());
    images[0] = canonical.after[0];
    targets[0] = targets[1]; // Duplicate or unordered observed participant.
    expect_rejected(binding, update, fixture, old_head, journal, target_span, image_span());
    targets = journal.targets;

    auto foreign = journal;
    foreign.payloads[0][200] ^= 1;
    stamp_payload_tag(foreign, binding, 0); // Both JBD2 CRCs remain correct.
    const auto codec = jbd2::Codec::open(foreign.journal_head,
        u32(binding.layout.journal_blocks), binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero);
    CHECK(codec && codec.value().descriptor(foreign.descriptor, foreign.sequence,
        [](const jbd2::Tag &) { return Status{}; }));
    expect_rejected(binding, update, fixture, old_head, foreign, target_span, image_span());

    foreign = journal;
    big32(foreign.commit, 52, update.seconds + 1);
    stamp_commit(foreign, binding); // A CRC-correct but noncanonical timestamp.
    CHECK(codec.value().commit(foreign.commit, foreign.sequence));
    expect_rejected(binding, update, fixture, old_head, foreign, target_span, image_span());

    foreign = journal;
    auto &changed_table = foreign.payloads[1];
    const usize links_at = slot(update.source) + 26;
    store16(changed_table, links_at, u16(short_value(changed_table, links_at) + 1));
    stamp_inode(changed_table, update.source, binding);
    stamp_payload_tag(foreign, binding, 1); // Ext4 inode and JBD2 CRCs both pass.
    CHECK(codec.value().descriptor(foreign.descriptor, foreign.sequence,
        [](const jbd2::Tag &) { return Status{}; }));
    expect_rejected(binding, update, fixture, old_head, foreign, target_span, image_span());

    foreign = journal;
    std::swap(foreign.targets[0], foreign.targets[1]);
    big32(foreign.descriptor, 12, foreign.targets[0]);
    big32(foreign.descriptor, 44, foreign.targets[1]);
    stamp_descriptor(foreign, binding);
    expect_rejected(binding, update, fixture, old_head, foreign, target_span, image_span());

    foreign = journal;
    foreign.payloads[0][200] ^= 1; // Invalid JBD2 payload CRC.
    expect_rejected(binding, update, fixture, old_head, foreign, target_span, image_span());
    expect_rejected(binding, update, fixture, old_head, journal, target_span,
                    std::span<const Block>{images}.first(journal.count - 1));
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
    auto filled_binding = *filled_admission.info();
    auto empty_binding = *empty_admission.info();
    filled_binding.needs_recovery = empty_binding.needs_recovery = true;
    filled_binding.empty_log = empty_binding.empty_log = true;
    const Block filled_head = load(filled, u32(filled_binding.layout.journal_first_block));
    const Block empty_head = load(empty, u32(empty_binding.layout.journal_first_block));
    Fixture base(filled, filled_binding); dirty(base);
    Fixture empty_file(empty, empty_binding); dirty(empty_file);
    const usize filled_reads = filled_disk.reads, empty_reads = empty_disk.reads;

    exercise(filled_binding, base, base.replacement("basic-note", "second-note"), filled_head, 4);
    exercise(filled_binding, base, base.unlink("basic-note"), filled_head, 4);
    exercise(empty_binding, empty_file, empty_file.unlink("empty-note"), empty_head, 4);

    Fixture empty_source(filled, filled_binding); dirty(empty_source);
    checked_empty_regular(empty_source, 14);
    exercise(filled_binding, empty_source,
             empty_source.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture empty_victim(filled, filled_binding); dirty(empty_victim);
    checked_empty_regular(empty_victim, 15);
    exercise(filled_binding, empty_victim,
             empty_victim.replacement("basic-note", "second-note"), filled_head, 4);
    Fixture both_empty(filled, filled_binding); dirty(both_empty);
    checked_empty_regular(both_empty, 14); checked_empty_regular(both_empty, 15);
    exercise(filled_binding, both_empty,
             both_empty.replacement("basic-note", "second-note"), filled_head, 4);

    Fixture nonlast(filled, filled_binding); dirty(nonlast);
    nonlast.destination = nonlast.source;
    nonlast.destination_block = nonlast.source_block;
    store16(nonlast.source, slot(14) + 26, 2); nonlast.stamp_inode(nonlast.source, 14);
    nonlast.destination = nonlast.source;
    nonlast.add_alias(14, "second-note", "basic-link");
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

    Fixture same(filled, filled_binding); dirty(same);
    same.destination_number = same.source_number;
    same.destination_block = same.source_block;
    store16(same.source, slot(14) + 26, 2); same.stamp_inode(same.source, 14);
    same.destination = same.source;
    same.add_alias(14, "second-note", "same-note");
    Ext4NamespaceJournalScratch no_op_scratch{};
    Ext4NamespaceJournalPlan no_op{};
    const auto same_update = same.replacement("basic-note", "same-note");
    CHECK(Ext4NamespaceJournalPlanner::plan(filled_binding, same_update, same.images(),
        filled_head, no_op_scratch, no_op) && no_op.no_op && !no_op.count);
    std::array<u32, 5> no_targets{}; std::array<Block, 5> no_images{};
    expect_rejected(filled_binding, same_update, same, filled_head, no_op,
                    std::span<const u32>{no_targets}.first(0),
                    std::span<const Block>{no_images}.first(0));

    Ext4NamespaceJournalScratch base_scratch{};
    Ext4NamespaceJournalPlan base_journal{};
    const auto base_update = base.replacement("basic-note", "second-note");
    CHECK(Ext4NamespaceJournalPlanner::plan(filled_binding, base_update, base.images(),
        filled_head, base_scratch, base_journal));
    const auto &planned = base_scratch.namespace_plan;
    std::array<Block, 5> final_homes{};
    for (usize index = 0; index < planned.count; ++index) final_homes[index] = planned.after[index];
    const auto final_targets = std::span<const u32>{base_journal.targets}.first(base_journal.count);
    const auto final_images = std::span<const Block>{final_homes}.first(base_journal.count);
    Fixture wrong_version(filled, filled_binding); dirty(wrong_version);
    store32(wrong_version.source, slot(14) + 36, value(wrong_version.source, slot(14) + 36) + 1);
    wrong_version.stamp_inode(wrong_version.source, 14);
    wrong_version.destination = wrong_version.source;
    expect_rejected(filled_binding, base_update, wrong_version, filled_head,
                    base_journal, final_targets, final_images);
    Fixture unallocated(filled, filled_binding); dirty(unallocated);
    constexpr u32 old_data_local = 535 - 1;
    unallocated.block_bitmaps[0][old_data_local / 8] &= u8(~(1U << (old_data_local % 8)));
    store16(unallocated.descriptors, 12, u16(short_value(unallocated.descriptors, 12) + 1));
    store32(unallocated.super, 12, value(unallocated.super, 12) + 1);
    store32(unallocated.super, 1020,
        jbd2::detail::crc32c(~u32{0}, std::span<const u8>{unallocated.super}.first(1020)));
    unallocated.stamp_block_bitmap(0);
    expect_rejected(filled_binding, base_update, unallocated, filled_head,
                    base_journal, final_targets, final_images);
    Fixture old_orphan(filled, filled_binding); dirty(old_orphan);
    store32(old_orphan.super, 232, 15);
    store32(old_orphan.super, 1020,
        jbd2::detail::crc32c(~u32{0}, std::span<const u8>{old_orphan.super}.first(1020)));
    expect_rejected(filled_binding, base_update, old_orphan, filled_head,
                    base_journal, final_targets, final_images);

    CHECK(filled_disk.reads == filled_reads && empty_disk.reads == empty_reads &&
          !filled_disk.writes && !filled_disk.flushes &&
          !empty_disk.writes && !empty_disk.flushes);
    std::printf("PASS explicit-witness namespace replay mixtures=%zu rejected=%zu no_device_io=1 cold_authority=0\n",
                mixtures, rejected);
}
