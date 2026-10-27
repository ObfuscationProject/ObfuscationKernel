#include "ext4_namespace_journal_fixture.hpp"
#include <ok/fs/ext4/recovery.hpp>

using namespace namespace_journal_model;
namespace
{
void big32(Block &block, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) block[at + index] = u8(value >> (24 - index * 8)); }
void stamp_journal(Block &head)
{ big32(head, 252, 0); big32(head, 252, jbd2::detail::crc32c(~u32{0}, head)); }
void dirty(Fixture &fixture)
{
    store32(fixture.super, 96, value(fixture.super, 96) | 4);
    store32(fixture.super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{fixture.super}.first(1020)));
}
void check_records(const Ext4RecoveryInfo &binding, Fixture &fixture, Ext4NamespaceUpdate update,
                   const Block &head, bool expected_orphan)
{
    Ext4NamespaceJournalScratch scratch{};
    Ext4NamespaceJournalPlan plan{};
    const auto prepared = Ext4NamespaceJournalPlanner::plan(binding, update, fixture.images(), head, scratch, plan);
    if (!prepared)
        std::fprintf(stderr, "namespace journal status: %.*s\n", int(prepared.message().size()), prepared.message().data());
    CHECK(prepared);
    CHECK(plan.ready && !plan.no_op && plan.count == scratch.namespace_plan.count);
    CHECK(scratch.namespace_plan.orphan_published == expected_orphan);
    CHECK(plan.sequence == binding.journal_sequence + 1);
    CHECK(jbd2::detail::be32(plan.journal_head, 24) == plan.sequence);
    CHECK(jbd2::detail::be32(plan.journal_head, 28) == 1);
    CHECK(jbd2::detail::all_zero(plan.end));
    const auto codec = jbd2::Codec::open(plan.journal_head, u32(binding.layout.journal_blocks),
        binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero); CHECK(codec);
    usize count = 0;
    CHECK(codec.value().descriptor(plan.descriptor, plan.sequence, [&](const jbd2::Tag &tag) -> Status {
        CHECK(count < plan.count && tag.target_block == plan.targets[count]);
        CHECK(codec.value().payload(plan.payloads[count], plan.sequence, tag));
        auto restored = plan.payloads[count];
        if (tag.restore_magic) big32(restored, 0, jbd2::magic);
        CHECK(restored == scratch.namespace_plan.after[count]);
        ++count; return {};
    }));
    CHECK(count == plan.count);
    const auto commit = codec.value().commit(plan.commit, plan.sequence);
    CHECK(commit && commit.value().seconds == update.seconds && commit.value().nanoseconds == update.nanoseconds);
    const std::array directories{fixture.directory_block};
    const jbd2::ReplayPlan replay{std::span<const jbd2::ReplayAction>{scratch.actions}.first(plan.count),
        1, plan.sequence + 1, 12, 0, false, false};
    const auto classified = Ext4ReplayShapePlanner::classify(binding, {directories}, replay);
    CHECK(classified && classified.value().includes(Ext4ReplayFamily::namespace_change));
    if (expected_orphan) CHECK(!classified.value().includes(Ext4ReplayFamily::bounded_data_allocation));
    auto bad = plan.descriptor; bad[100] ^= 1;
    CHECK(!codec.value().descriptor(bad, plan.sequence, [](const jbd2::Tag &) -> Status { return {}; }));
    bad = plan.commit; bad[100] ^= 1; CHECK(!codec.value().commit(bad, plan.sequence));
    CHECK(codec.value().descriptor(plan.descriptor, plan.sequence, [&](const jbd2::Tag &tag) -> Status {
        auto payload = plan.payloads[0]; payload[100] ^= 1;
        if (tag.target_block == plan.targets[0]) CHECK(!codec.value().payload(payload, plan.sequence, tag));
        return {};
    }));
}
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Image bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() == 32 * 1024 * 1024);
    MemoryDisk disk(bytes); Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, bytes.size() / 512}));
    auto binding = *admission.info(); binding.needs_recovery = true; binding.empty_log = true;
    const Block head = load(bytes, u32(binding.layout.journal_first_block));
    Fixture fixture(bytes, binding); dirty(fixture);
    const usize reads = disk.reads;
    check_records(binding, fixture, fixture.replacement("basic-note", "second-note"), head, true);
    auto revoke_head = head;
    big32(revoke_head, 40, jbd2::feature_checksum_v3 | jbd2::feature_revoke); stamp_journal(revoke_head);
    check_records(binding, fixture, fixture.replacement("basic-note", "second-note"), revoke_head, true);
    Fixture wide(bytes, binding, 12, 14); dirty(wide);
    check_records(binding, wide, wide.replacement("seed", "basic-note"), head, true);
    auto last_sequence = binding; last_sequence.journal_sequence = 0xfffffffcU;
    auto last_head = head; big32(last_head, 24, last_sequence.journal_sequence); stamp_journal(last_head);
    Fixture last_fixture(bytes, last_sequence); dirty(last_fixture);
    check_records(last_sequence, last_fixture, last_fixture.replacement("basic-note", "second-note"), last_head, true);
    auto multiple = fixture;
    store16(multiple.destination, slot(15) + 26, 2);
    multiple.stamp_inode(multiple.destination, 15); multiple.source = multiple.destination;
    check_records(binding, multiple, multiple.replacement("basic-note", "second-note"), head, false);
    auto removed = fixture; removed.destination = removed.source; removed.destination_block = removed.source_block;
    check_records(binding, removed, removed.unlink("basic-note"), head, true);
    auto escaped = fixture;
    big32(escaped.parent, 0, jbd2::magic);
    check_records(binding, escaped, escaped.replacement("basic-note", "second-note"), head, true);
    auto same = fixture;
    same.destination_number = same.source_number;
    same.destination_block = same.source_block;
    store16(same.source, slot(14) + 26, 2); same.stamp_inode(same.source, 14);
    same.destination = same.source;
    same.add_alias(14, "second-note", "same-note");
    Ext4NamespaceJournalScratch scratch{}; Ext4NamespaceJournalPlan plan{};
    CHECK(Ext4NamespaceJournalPlanner::plan(binding, same.replacement("basic-note", "same-note"),
        same.images(), head, scratch, plan));
    CHECK(plan.ready && plan.no_op && !plan.count && !plan.sequence);
    CHECK(jbd2::detail::all_zero(plan.journal_head) && jbd2::detail::all_zero(plan.descriptor));
    const auto reject = [&](Ext4RecoveryInfo changed_binding, Block journal, Ext4NamespaceUpdate update) {
        plan.ready = true; plan.count = 5;
        CHECK(!Ext4NamespaceJournalPlanner::plan(changed_binding, update, fixture.images(), journal, scratch, plan));
        CHECK(!plan.ready && !plan.count && !plan.sequence);
    };
    auto changed = binding; changed.needs_recovery = false;
    reject(changed, head, fixture.replacement("basic-note", "second-note"));
    changed = binding; changed.empty_log = false;
    reject(changed, head, fixture.replacement("basic-note", "second-note"));
    changed = binding; ++changed.journal_sequence;
    reject(changed, head, fixture.replacement("basic-note", "second-note"));
    auto corrupt = head; corrupt[80] ^= 1;
    reject(binding, corrupt, fixture.replacement("basic-note", "second-note"));
    corrupt = head; big32(corrupt, 28, 1); stamp_journal(corrupt);
    reject(binding, corrupt, fixture.replacement("basic-note", "second-note"));
    corrupt = head; big32(corrupt, 24, 0xfffffffdU); stamp_journal(corrupt);
    changed = binding; changed.journal_sequence = 0xfffffffdU;
    reject(changed, corrupt, fixture.replacement("basic-note", "second-note"));
    corrupt = head; big32(corrupt, 20, 2); stamp_journal(corrupt);
    reject(binding, corrupt, fixture.replacement("basic-note", "second-note"));
    corrupt = head; big32(corrupt, 40, jbd2::feature_checksum_v3 | jbd2::feature_revoke | jbd2::feature_64bit);
    stamp_journal(corrupt); reject(binding, corrupt, fixture.replacement("basic-note", "second-note"));
    auto bad_update = fixture.replacement("basic-note", "second-note"); ++bad_update.source_generation;
    reject(binding, head, bad_update);
    CHECK(disk.reads == reads && !disk.writes && !disk.flushes);
    std::puts("PASS namespace journal model: canonical records, no-op, escape, CRC and fail-closed admission");
}
