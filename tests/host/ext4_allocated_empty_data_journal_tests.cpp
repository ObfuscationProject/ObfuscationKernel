#include "ext4_zero_truncate_test_support.hpp"
#include <ok/fs/ext4_allocated_empty_data_journal.hpp>
#include <cstdio>
#include <cstdlib>
using namespace zero_fixture;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "allocated empty journal:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
namespace
{
Ext4AllocatedEmptyDataScratch planning{};
Ext4AllocatedEmptyDataPlan plan{}, changed{};
Ext4AllocatedEmptyDataJournalFormatScratch workspace{};
Ext4AllocatedEmptyDataJournalFormat formatted{}, saved_format{}, altered{};
Ext4AllocatedEmptyDataJournalScratch verification{};
Ext4AllocatedEmptyDataJournalResult result{};
Block head{}, saved_head{}, logical{};
std::array<Block, 4> homes{};
usize positive{}, refusal{}, mixtures{}, repaired_commit{}, repaired_after{}, repaired_descriptor{};
void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void stamp(Block &bytes, usize at, u32 seed)
{ be32(bytes, at, 0); be32(bytes, at, jbd2::detail::crc32c(seed, bytes)); }
std::span<u8> node(Fixture &f)
{ return std::span<u8>{f.inode_table}.subspan(((f.inode - 1) % 4) * 256, 256); }
void prepare(Fixture &f, u32 slot = 0, u32 size = 73, u32 sequence = 70, u64 version = 42, bool revoke = false)
{
    std::array<u8, 256> old{};
    for (usize at = 0; at < 256; ++at) old[at] = f.inode_table[at];
    for (usize at = 0; at < 1024; ++at) f.inode_table[at] = u8((at * 11 + 29) % 251);
    f.inode += slot;
    for (usize at = 0; at < 256; ++at) node(f)[at] = old[at];
    put32(node(f), 36, u32(version)); put32(node(f), 152, u32(version >> 32));
    if (slot) be32(f.inode_table, 0, jbd2::magic);
    stamp_inode(node(f), f.inode, f.seed);
    f.inode_bitmap.fill(0); f.inode_bitmap[(f.inode - 1) / 8] |= u8(1U << ((f.inode - 1) % 8));
    put16(f.descriptors, 26, u16(jbd2::detail::crc32c(f.seed, std::span<const u8>{f.inode_bitmap}.first(32))));
    f.refresh_descriptor_crc(0);
    const Ext4AllocatedEmptyDataUpdate update{f.inode, 9, size, 1700000222, 987654321};
    const Ext4AllocatedEmptyDataSource source{f.group, f.physical, f.table_block, f.superblock,
        f.descriptors, f.bitmap, f.inode_bitmap, f.inode_table};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(f.binding, update, source, planning, plan));
    head = {};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, sequence);
    be32(head, 40, jbd2::feature_checksum_v3 | (revoke ? jbd2::feature_revoke : 0));
    for (usize at = 0; at < f.binding.uuid.size(); ++at) head[48 + at] = f.binding.uuid[at];
    be32(head, 64, 1); head[80] = 4; stamp(head, 252, ~u32{0});
    f.binding.journal_sequence = sequence; saved_head = head;
}
Ext4RecoveryInfo active(const Fixture &f)
{ auto value = f.binding; value.empty_log = false; value.journal_sequence = formatted.active_sequence; return value; }
void format_ok(Fixture &f, bool escaped)
{
    const auto old_before = plan.before, old_after = plan.after;
    CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(f.binding, plan, head, f.inode_bitmap, workspace, formatted));
    CHECK(formatted.ready && formatted.active_sequence == f.binding.journal_sequence + 1 &&
        formatted.next_sequence == f.binding.journal_sequence + 2 && head == saved_head);
    CHECK(plan.before == old_before && plan.after == old_after && jbd2::detail::all_zero(formatted.end));
    CHECK(jbd2::detail::be32(formatted.commit, 52) == plan.update.seconds &&
        jbd2::detail::be32(formatted.commit, 56) == plan.update.nanoseconds);
    CHECK(jbd2::detail::be32(formatted.empty_head, 24) == formatted.next_sequence && !jbd2::detail::be32(formatted.empty_head, 28));
    CHECK(bool(jbd2::detail::be32(formatted.descriptor, 80) & jbd2::flag_escape) == escaped);
    const auto binding = active(f);
    const Ext4AllocatedEmptyDataJournalRecords records{formatted.head, formatted.descriptor,
        formatted.commit, formatted.end, formatted.payloads};
    for (u8 mask = 0; mask < 16; ++mask)
    {
        for (usize home = 0; home < 4; ++home) homes[home] = mask & (1U << home) ? plan.after[home] : plan.before[home];
        CHECK(Ext4AllocatedEmptyDataJournalVerifier::verify(binding, records, f.inode_bitmap, homes, verification, result));
        CHECK(result.complete && result.after_mask == mask && result.active_sequence == formatted.active_sequence &&
            result.next_sequence == formatted.next_sequence && result.witness.old_version == plan.old_version &&
            result.witness.new_version == plan.new_version && verification.decoded.before == plan.before &&
            verification.decoded.after == plan.after && verification.decoded.targets == plan.targets);
        ++positive; ++mixtures;
    }
    ++positive;
}
void verify_refused(const Fixture &f, const Ext4AllocatedEmptyDataJournalFormat &input)
{
    const auto binding = active(f);
    const Ext4AllocatedEmptyDataJournalRecords records{input.head, input.descriptor, input.commit, input.end, input.payloads};
    result.complete = true; result.active_sequence = 99;
    CHECK(!Ext4AllocatedEmptyDataJournalVerifier::verify(binding, records, f.inode_bitmap, plan.before, verification, result));
    CHECK(!result.complete && !result.active_sequence && !result.next_sequence && !result.witness.sequence); ++refusal;
}
void format_refused(const Fixture &f, const Ext4RecoveryInfo &binding, const Ext4AllocatedEmptyDataPlan &input)
{
    formatted.head.fill(19); formatted.ready = true;
    CHECK(!Ext4AllocatedEmptyDataJournalFormatter::format(binding, input, head, f.inode_bitmap, workspace, formatted));
    CHECK(!formatted.ready && !formatted.active_sequence && !formatted.next_sequence &&
        jbd2::detail::all_zero(formatted.head) && jbd2::detail::all_zero(formatted.descriptor)); ++refusal;
}
void corruptions()
{
    for (bool cross : {false, true})
    {
        Fixture f(cross); prepare(f, 3); format_ok(f, true); saved_format = formatted;
        for (usize at = 0; at < 1024; ++at)
        {
            if (at >= 16 && at < 20) continue;
            altered = saved_format; altered.commit[at] ^= 1; stamp(altered.commit, 16, f.seed);
            verify_refused(f, altered); ++repaired_commit;
        }
        for (usize at = 0; at < 1020; ++at)
        {
            altered = saved_format; altered.descriptor[at] ^= 1; stamp(altered.descriptor, 1020, f.seed);
            verify_refused(f, altered); ++repaired_descriptor;
        }
        for (usize home = 0; home < 4; ++home) for (usize at = 0; at < 1024; ++at)
        {
            altered = saved_format; logical = plan.after[home]; logical[at] ^= 1;
            put32(altered.descriptor, 224 + home * 4, jbd2::detail::crc32c(~u32{0}, logical));
            const usize tag = home ? 44 + (home - 1) * 16 : 12;
            const bool escaped = jbd2::detail::be32(logical, 0) == jbd2::magic;
            be32(altered.descriptor, tag + 4, (home ? jbd2::flag_same_uuid : 0) |
                (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0));
            altered.payloads[home] = logical; if (escaped) be32(altered.payloads[home], 0, 0);
            be32(altered.descriptor, tag + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(f.seed,
                std::span<const u8>{altered.descriptor}.subspan(8, 4)), altered.payloads[home]));
            stamp(altered.descriptor, 1020, f.seed); verify_refused(f, altered); ++repaired_after;
        }
        for (usize kind = 0; kind < 7; ++kind)
        {
            altered = saved_format;
            if (kind == 0) altered.end[900] = 1;
            if (kind == 1) altered.commit.fill(0);
            if (kind == 2) altered.commit[16] ^= 1;
            if (kind == 3) altered.descriptor[1020] ^= 1;
            if (kind == 4) { be32(altered.head, 28, 2); stamp(altered.head, 252, ~u32{0}); }
            if (kind == 5) { be32(altered.head, 24, formatted.active_sequence + 1); stamp(altered.head, 252, ~u32{0}); }
            if (kind == 6) altered.payloads[0][0] ^= 1;
            verify_refused(f, altered);
        }
        formatted = saved_format;
    }
}
void negatives()
{
    for (u32 field = 0; field < 10; ++field)
    {
        Fixture f; prepare(f); changed = plan;
        if (field == 0) ++changed.update.inode;
        if (field == 1) ++changed.update.generation;
        if (field == 2) changed.update.size = 0;
        if (field == 3) ++changed.physical;
        if (field == 4) ++changed.group;
        if (field == 5) ++changed.old_version;
        if (field == 6) ++changed.targets[3];
        if (field == 7) changed.before[3][900] ^= 1;
        if (field == 8) changed.after[3][900] ^= 1;
        if (field == 9) changed.count = 3;
        format_refused(f, f.binding, changed);
    }
    for (u32 field = 0; field < 8; ++field)
    {
        Fixture f; prepare(f); auto binding = f.binding;
        if (field == 0) binding.needs_recovery = false;
        if (field == 1) binding.empty_log = false;
        if (field == 2) ++binding.journal_sequence;
        if (field == 3) binding.uuid[0] ^= 1;
        if (field == 4) binding.layout.block_size = 4096;
        if (field == 5) binding.metadata_count = 0;
        if (field == 6) { be32(head, 28, 1); stamp(head, 252, ~u32{0}); }
        if (field == 7) { be32(head, 40, jbd2::feature_checksum_v3 | jbd2::feature_64bit); stamp(head, 252, ~u32{0}); }
        format_refused(f, binding, plan);
    }
    for (u32 sequence : {0U, 0xfffffffdU, 0xfffffffeU, 0xffffffffU})
    { Fixture f; prepare(f, 0, 73, sequence); format_refused(f, f.binding, plan); }
    {
        Fixture f; prepare(f); formatted = {}; formatted.head = head; saved_format = formatted;
        CHECK(!Ext4AllocatedEmptyDataJournalFormatter::format(f.binding, plan, formatted.head, f.inode_bitmap, workspace, formatted));
        CHECK(formatted.head == saved_format.head && formatted.payloads == saved_format.payloads); ++refusal;
        workspace.verification.record.fill(17); const auto saved = workspace.verification.record;
        CHECK(!Ext4AllocatedEmptyDataJournalFormatter::format(f.binding, plan, workspace.verification.record, f.inode_bitmap, workspace, formatted));
        CHECK(workspace.verification.record == saved); ++refusal;
        format_ok(f, false); const auto binding = active(f);
        const Ext4AllocatedEmptyDataJournalRecords short_records{formatted.head, formatted.descriptor,
            formatted.commit, formatted.end, std::span<const Block>{formatted.payloads}.first(3)};
        CHECK(!Ext4AllocatedEmptyDataJournalVerifier::verify(binding, short_records, f.inode_bitmap, plan.before, verification, result));
        CHECK(!result.complete); ++refusal;
        const Ext4AllocatedEmptyDataJournalRecords records{formatted.head, formatted.descriptor, formatted.commit, formatted.end, formatted.payloads};
        CHECK(!Ext4AllocatedEmptyDataJournalVerifier::verify(binding, records, f.inode_bitmap,
            std::span<const Block>{plan.before}.first(3), verification, result)); CHECK(!result.complete); ++refusal;
        verification.record.fill(23); const auto sentinel = verification.record;
        const Ext4AllocatedEmptyDataJournalRecords alias{verification.record, formatted.descriptor, formatted.commit, formatted.end, formatted.payloads};
        CHECK(!Ext4AllocatedEmptyDataJournalVerifier::verify(binding, alias, f.inode_bitmap, plan.before, verification, result));
        CHECK(verification.record == sentinel); ++refusal;
    }
}
} // namespace
int main()
{
    for (bool cross : {false, true}) for (bool revoke : {false, true}) for (u32 slot = 0; slot < 4; ++slot)
        for (u32 size : {1U, 1024U}) for (u32 sequence : {1U, 70U, 0xfffffffcU})
            for (u64 version : {u64{0}, u64{0xffffffff}, ~u64{0} - 1})
            { Fixture f(cross); prepare(f, slot, size, sequence, version, revoke); format_ok(f, slot != 0); }
    corruptions(); negatives();
    std::printf("PASS hosted allocated-empty canonical journal positive=%zu refusal=%zu mixtures=%zu repaired_commit=%zu repaired_descriptor=%zu repaired_after=%zu\n",
        positive, refusal, mixtures, repaired_commit, repaired_descriptor, repaired_after);
    return 0;
}
