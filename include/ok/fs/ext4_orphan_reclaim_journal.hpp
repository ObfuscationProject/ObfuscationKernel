#pragma once

#include "ok/fs/ext4_orphan_reclaim_witness.hpp"

namespace ok::fs
{
struct Ext4OrphanReclaimJournalPlan
{
    using Block = Ext4OrphanBlock;
    Block journal_head{}, descriptor{}, commit{}, end{};
    std::array<Block, Ext4OrphanReclaimPlan::capacity> payloads{};
    std::array<u32, Ext4OrphanReclaimPlan::capacity> targets{};
    usize count{};
    u32 sequence{};
    bool ready{};
};

struct Ext4OrphanReclaimJournalScratch
{
    using Block = Ext4OrphanBlock;
    std::array<jbd2::ReplayAction, Ext4OrphanReclaimPlan::capacity> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    std::array<Block, Ext4OrphanReclaimPlan::capacity> before{};
    Block record{}, payload{};
};

// Pure, bounded format preparation for one head-reclaim transaction. It does
// not read a device, confer owner authority, or authorize runtime/cold replay.
class Ext4OrphanReclaimJournalPlanner final
{
    using Block = Ext4OrphanBlock;
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (24 - i * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    {
        be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(type));
        be32(bytes, 8, sequence);
    }
    struct Records
    {
        const Ext4OrphanReclaimJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024)
                return Status::invalid_argument("orphan reclaim journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2)
                record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("orphan reclaim journal model read bounds");
            for (usize i = 0; i < bytes.size(); ++i) bytes[i] = (*record)[i];
            return {};
        }
    };

  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimPlan &reclaim, const Block &old_journal_head,
        u32 seconds, u32 nanoseconds, Ext4OrphanReclaimJournalScratch &scratch,
        Ext4OrphanReclaimJournalPlan &output)
    {
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log ||
            seconds > 0x7fffffffU || nanoseconds >= 1000000000U)
            return Status::unsupported("orphan reclaim journal admission or time");
        const auto codec = jbd2::Codec::open(old_journal_head,
            u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.first_block != 1 ||
            journal.start_block || journal.sequence != binding.journal_sequence ||
            journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            (reclaim.count != 4 && reclaim.count != 5) ||
            journal.max_blocks <= reclaim.count + 3)
            return Status::unsupported("orphan reclaim journal ring or record capacity");
        output.count = reclaim.count;
        output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                         u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = reclaim.targets[index];
            auto &payload = output.payloads[index];
            payload = reclaim.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { be32(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index ? 44 + (index - 1) * 16 : 12;
            be32(output.descriptor, at, output.targets[index]);
            be32(output.descriptor, at + 4, flags);
            be32(output.descriptor, at + 12,
                 jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        if (auto status = Ext4OrphanReclaimWitnessCodec::encode(
                binding, reclaim, output.sequence, output.descriptor); !status)
            return reject(status);
        be32(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        be32(output.commit, 52, seconds); be32(output.commit, 56, nanoseconds);
        be32(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        be32(output.journal_head, 24, output.sequence);
        be32(output.journal_head, 28, 1);
        be32(output.journal_head, 252, 0);
        be32(output.journal_head, 252,
             jbd2::detail::crc32c(~u32{0}, output.journal_head));
        Ext4OrphanReclaimWitness witness{};
        if (auto status = Ext4OrphanReclaimWitnessCodec::decode(output.descriptor,
                binding.uuid, std::span<const Block>{reclaim.after}.first(output.count),
                std::span<Block>{scratch.before}.first(output.count), witness); !status)
            return reject(status);
        if (witness.sequence != output.sequence || witness.inode != reclaim.inode ||
            witness.old_generation != reclaim.old_generation ||
            witness.retained_generation != reclaim.retained_generation ||
            witness.next_head != reclaim.next_head ||
            witness.released_blocks != reclaim.released_blocks ||
            witness.count != reclaim.count)
            return reject(Status::fault("orphan reclaim reconstructed witness identity"));
        for (usize i = 0; i < output.count; ++i)
            if (witness.targets[i] != reclaim.targets[i] ||
                scratch.before[i] != reclaim.before[i])
                return reject(Status::fault("orphan reclaim reconstructed home differs"));
        for (usize i = 0; i < reclaim.released_blocks; ++i)
            if (witness.released_mapping[i] != reclaim.released_mapping[i])
                return reject(Status::fault("orphan reclaim reconstructed mapping differs"));
        const auto new_codec = jbd2::Codec::open(output.journal_head,
            u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, Ext4OrphanReclaimPlan::capacity> allowed{};
        for (usize i = 0; i < output.count; ++i) allowed[i] = {output.targets[i], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks,
             std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("orphan reclaim generated journal shape"));
        for (usize i = 0; i < replay.actions.size(); ++i)
        {
            const auto &action = replay.actions[i];
            if (action.target_block != output.targets[i] ||
                action.journal_block != i + 2 || action.sequence != output.sequence ||
                action.transaction_order)
                return reject(Status::fault("orphan reclaim generated action identity"));
            for (usize byte = 0; byte < 1024; ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ?
                    u8(jbd2::magic >> (24 - byte * 8)) : output.payloads[i][byte];
                if (restored != reclaim.after[i][byte])
                    return reject(Status::fault("orphan reclaim generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};
} // namespace ok::fs
