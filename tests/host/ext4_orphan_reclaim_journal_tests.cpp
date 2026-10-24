#include "ok/fs/ext4_orphan_reclaim_journal.hpp"
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
            for (u32 i = 0; i < 512; ++i) groups[group].push_back(load(image, first + i));
        }
        for (usize i = 0; i < fixture.count; ++i)
        {
            const auto &member = fixture.images[i];
            const u32 group = (member.number - 1) / 2048;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            groups[group][member.table_block - first] = member.table;
        }
    }
    Ext4OrphanOwnershipSource source(const Fixture &fixture) const
    {
        std::array<std::span<const Block>, 4> spans{};
        for (usize i = 0; i < spans.size(); ++i) spans[i] = groups[i];
        return {fixture.source(), spans};
    }
};
struct JournalRecords
{
    const Ext4OrphanReclaimJournalPlan &plan;
    Status read_block(u32 number, std::span<u8> bytes)
    {
        const Block *record = nullptr;
        if (number == 0) record = &plan.journal_head;
        else if (number == 1) record = &plan.descriptor;
        else if (number >= 2 && number < plan.count + 2)
            record = &plan.payloads[number - 2];
        else if (number == plan.count + 2) record = &plan.commit;
        else if (number == plan.count + 3) record = &plan.end;
        if (!record || bytes.size() != 1024)
            return Status::io_error("orphan reclaim test record bounds");
        for (usize i = 0; i < bytes.size(); ++i) bytes[i] = (*record)[i];
        return {};
    }
};
bool parser_accepts(const Ext4RecoveryInfo &binding,
    const Ext4OrphanReclaimJournalPlan &journal)
{
    const auto codec = jbd2::Codec::open(journal.journal_head,
        u32(binding.layout.journal_blocks), binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero);
    if (!codec) return false;
    std::array<jbd2::BlockRange, 5> allowed{};
    for (usize i = 0; i < journal.count; ++i)
        allowed[i] = {journal.targets[i], 1};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Block record{}, payload{};
    JournalRecords records{journal};
    const auto parsed = jbd2::build_replay_plan(codec.value(), records,
        {binding.layout.filesystem_blocks,
         std::span<const jbd2::BlockRange>{allowed}.first(journal.count)},
        {record, payload, actions, revocations});
    return parsed && parsed.value().committed_transactions == 1 &&
        parsed.value().actions.size() == journal.count;
}

Ext4OrphanReclaimPlan make(const Ext4RecoveryInfo &binding,
    const Fixture &fixture, const Tables &tables, u32 inode)
{
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanReclaimPlan plan{};
    CHECK(Ext4OrphanReclaimPlanner::plan(
        binding, tables.source(fixture), scratch, inode, 0, plan));
    CHECK(plan.inode == inode && plan.count == (plan.released_blocks ? 5U : 4U));
    return plan;
}
void stamp_descriptor(Block &descriptor, const std::array<u8, 16> &uuid)
{
    for (usize i = 1020; i < 1024; ++i) descriptor[i] = 0;
    const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
    const u32 crc = jbd2::detail::crc32c(seed, descriptor);
    for (usize i = 0; i < 4; ++i) descriptor[1020 + i] = u8(crc >> (24 - i * 8));
}
void reject_decode(const Block &descriptor, const Ext4RecoveryInfo &binding,
    const Ext4OrphanReclaimPlan &plan, usize &rejections)
{
    std::array<Block, 5> before{};
    Ext4OrphanReclaimWitness witness{};
    const auto status = Ext4OrphanReclaimWitnessCodec::decode(descriptor,
        binding.uuid, std::span<const Block>{plan.after}.first(plan.count),
        std::span<Block>{before}.first(plan.count), witness);
    CHECK(!status && !witness.count);
    ++rejections;
}
void exercise(const Image &image, u32 inode, u32 next, usize expected_count,
    usize &successes, usize &rejections)
{
    MemoryDisk disk(image);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, image.size() / 512}));
    CHECK(admission.info());
    const auto &binding = *admission.info();
    CHECK(binding.needs_recovery && binding.empty_log);
    const usize reads = disk.reads;
    Fixture fixture(image, binding);
    fixture.add(image, inode, next);
    if (next) fixture.add(image, next, 0);
    fixture.head(inode);
    Tables tables(image, fixture);
    auto reclaim = make(binding, fixture, tables, inode);
    CHECK(reclaim.count == expected_count);
    const Block old_journal = load(image, u32(binding.layout.journal_first_block));
    Ext4OrphanReclaimJournalScratch scratch{};
    Ext4OrphanReclaimJournalPlan journal{};
    CHECK(Ext4OrphanReclaimJournalPlanner::plan(
        binding, reclaim, old_journal, 7, 13, scratch, journal));
    CHECK(journal.ready && journal.count == reclaim.count &&
          journal.sequence == binding.journal_sequence + 1);
    CHECK(jbd2::detail::be32(journal.commit, 52) == 7 &&
          jbd2::detail::be32(journal.commit, 56) == 13);
    CHECK(parser_accepts(binding, journal));
    std::array<Block, 5> before{};
    Ext4OrphanReclaimWitness witness{};
    CHECK(Ext4OrphanReclaimWitnessCodec::decode(journal.descriptor,
        binding.uuid, std::span<const Block>{reclaim.after}.first(reclaim.count),
        std::span<Block>{before}.first(reclaim.count), witness));
    CHECK(witness.uuid == binding.uuid && witness.inode == inode &&
          witness.old_generation == reclaim.old_generation &&
          witness.retained_generation == reclaim.retained_generation &&
          witness.next_head == next && witness.sequence == journal.sequence &&
          witness.released_blocks == reclaim.released_blocks &&
          witness.count == reclaim.count && witness.runs && witness.bytes);
    for (usize i = 0; i < reclaim.count; ++i)
        CHECK(before[i] == reclaim.before[i] && witness.targets[i] == reclaim.targets[i]);
    for (usize i = 0; i < reclaim.released_blocks; ++i)
        CHECK(witness.released_mapping[i] == reclaim.released_mapping[i]);
    CHECK(disk.reads == reads && !disk.writes && !disk.flushes);
    ++successes;
    const Block original_descriptor = journal.descriptor;
    auto broken_commit = journal;
    broken_commit.commit[16] ^= 1;
    CHECK(!parser_accepts(binding, broken_commit));
    ++rejections;
    auto broken_payload = journal;
    broken_payload.payloads[0][200] ^= 1;
    CHECK(!parser_accepts(binding, broken_payload));
    ++rejections;
    if (reclaim.released_blocks)
    {
        // Formatter-only escape case: the old and new sibling bytes agree,
        // but the logical table home starts with JBD2 magic.
        auto escaped = reclaim;
        for (usize i = 0; i < 4; ++i)
            escaped.before[0][i] = escaped.after[0][i] =
                u8(jbd2::magic >> (24 - i * 8));
        Ext4OrphanReclaimJournalPlan escaped_journal{};
        CHECK(Ext4OrphanReclaimJournalPlanner::plan(
            binding, escaped, old_journal, 7, 13, scratch, escaped_journal));
        CHECK(escaped_journal.ready && jbd2::detail::be32(escaped_journal.payloads[0], 0) == 0 &&
              (jbd2::detail::be32(escaped_journal.descriptor, 16) & jbd2::flag_escape));
        Ext4OrphanReclaimWitness escaped_witness{};
        CHECK(Ext4OrphanReclaimWitnessCodec::decode(escaped_journal.descriptor,
            binding.uuid, std::span<const Block>{escaped.after}.first(escaped.count),
            std::span<Block>{before}.first(escaped.count), escaped_witness));
        for (usize i = 0; i < escaped.count; ++i)
            CHECK(before[i] == escaped.before[i]);
    }

    Block altered = journal.descriptor;
    altered[145] ^= 1;
    stamp_descriptor(altered, binding.uuid);
    reject_decode(altered, binding, reclaim, rejections);
    altered = journal.descriptor;
    altered[176] ^= 1;
    stamp_descriptor(altered, binding.uuid);
    reject_decode(altered, binding, reclaim, rejections);
    altered = journal.descriptor;
    altered[180] ^= 1;
    stamp_descriptor(altered, binding.uuid);
    reject_decode(altered, binding, reclaim, rejections);
    altered = journal.descriptor;
    altered[304] = u8(reclaim.count);
    stamp_descriptor(altered, binding.uuid);
    reject_decode(altered, binding, reclaim, rejections);
    altered = journal.descriptor;
    altered[1020] ^= 1;
    reject_decode(altered, binding, reclaim, rejections);
    altered = journal.descriptor;
    altered[128] = 0;
    stamp_descriptor(altered, binding.uuid);
    reject_decode(altered, binding, reclaim, rejections);
    if (reclaim.released_blocks)
    {
        altered = journal.descriptor;
        altered[240] ^= 1;
        stamp_descriptor(altered, binding.uuid);
        reject_decode(altered, binding, reclaim, rejections);
    }

    auto after = reclaim.after;
    after[0][(inode == 12 ? 768U : 256U) + 200] ^= 1;
    Ext4OrphanReclaimWitness damaged{};
    CHECK(!Ext4OrphanReclaimWitnessCodec::decode(journal.descriptor, binding.uuid,
        std::span<const Block>{after}.first(reclaim.count),
        std::span<Block>{before}.first(reclaim.count), damaged));
    ++rejections;

    auto bad = reclaim;
    bad.released_mapping[0] = bad.released_blocks ? bad.released_mapping[0] + 1 : 1;
    journal = {};
    CHECK(!Ext4OrphanReclaimJournalPlanner::plan(
        binding, bad, old_journal, 7, 13, scratch, journal));
    CHECK(!journal.ready && !journal.count);
    ++rejections;

    if (reclaim.released_blocks)
    {
        // Alternating proposed bytes exceed the 716-byte descriptor patch
        // budget while the witnessed old inode and mapping remain valid.
        bad = reclaim;
        const usize at = slot(inode);
        for (usize i = 0; i < 256; ++i)
            if ((i < 100 || i >= 104) && (i < 160 || i >= 164))
                bad.after[0][at + i] = u8(bad.before[0][at + i] ^ (i & 1U));
        Block unchanged = original_descriptor;
        const auto overflow = Ext4OrphanReclaimWitnessCodec::encode(
            binding, bad, binding.journal_sequence + 1, unchanged);
        CHECK(!overflow && overflow.code() == StatusCode::unsupported);
        CHECK(unchanged == original_descriptor);
        Ext4OrphanReclaimJournalPlan too_large{};
        const auto rejected = Ext4OrphanReclaimJournalPlanner::plan(
            binding, bad, old_journal, 7, 13, scratch, too_large);
        CHECK(!rejected && rejected.code() == StatusCode::unsupported &&
              !too_large.ready && !too_large.count);
        ++rejections;
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream filled_input(argv[1], std::ios::binary);
    std::ifstream empty_input(argv[2], std::ios::binary);
    CHECK(filled_input && empty_input);
    const Image filled((std::istreambuf_iterator<char>(filled_input)), {});
    const Image empty((std::istreambuf_iterator<char>(empty_input)), {});
    CHECK(filled.size() == 32 * 1024 * 1024 && empty.size() == filled.size());
    usize successes = 0, rejections = 0;
    exercise(filled, 12, 14, 5, successes, rejections);
    exercise(empty, 14, 0, 4, successes, rejections);
    CHECK(successes == 2 && rejections >= 22);
    std::printf("PASS orphan reclaim journal witness: homes=5,4 complete_before=2 rejects=%zu no_device_io=1\n",
                rejections);
}
