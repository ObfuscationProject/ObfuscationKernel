#pragma once

#include "ok/fs/ext4_namespace_plan.hpp"

namespace ok::fs
{
struct Ext4NamespaceJournalScratch
{
    Ext4NamespacePlan namespace_plan{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4NamespaceBlock record{}, payload{};
};
struct Ext4NamespaceJournalPlan
{
    Ext4NamespaceBlock journal_head{}, descriptor{}, commit{}, end{};
    std::array<Ext4NamespaceBlock, 5> payloads{};
    std::array<u32, 5> targets{};
    usize count{};
    u32 sequence{};
    bool no_op{}, ready{};
};

// Immutable format preparation only. Scratch/output must be disjoint from
// source images and each other. No I/O, sealing, replay or mount authority.
class Ext4NamespaceJournalPlanner final
{
    using Block = Ext4NamespaceBlock;
    static void store(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { store(bytes, 0, jbd2::magic); store(bytes, 4, u32(type)); store(bytes, 8, sequence); }
    struct Records
    {
        const Ext4NamespaceJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("namespace journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2) record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("namespace journal model read bounds");
            for (usize index = 0; index < bytes.size(); ++index) bytes[index] = (*record)[index];
            return {};
        }
    };
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4NamespaceUpdate &update, const Ext4NamespaceSource &source,
        const Block &old_journal_head, Ext4NamespaceJournalScratch &scratch,
        Ext4NamespaceJournalPlan &output)
    {
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log)
            return Status::unsupported("namespace journal requires mounted dirty empty log");
        const auto codec = jbd2::Codec::open(old_journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.first_block != 1 || journal.start_block ||
            journal.sequence != binding.journal_sequence || journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("namespace journal ring, sequence or feature profile");
        auto &canonical = scratch.namespace_plan;
        if (const auto status = Ext4NamespacePlanner::plan(binding, update, source, canonical); !status)
            return status;
        if (canonical.no_op)
        { output.no_op = true; output.ready = true; return {}; }
        if (canonical.count < 2 || canonical.count > output.targets.size() ||
            journal.max_blocks <= canonical.count + 3)
            return Status::unsupported("namespace journal record capacity");
        output.count = canonical.count; output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                      u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = canonical.targets[index];
            auto &payload = output.payloads[index];
            payload = canonical.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { store(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index == 0 ? 12 : 44 + (index - 1) * 16;
            store(output.descriptor, at, output.targets[index]);
            store(output.descriptor, at + 4, flags);
            store(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        store(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        store(output.commit, 52, update.seconds); store(output.commit, 56, update.nanoseconds);
        store(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        store(output.journal_head, 24, output.sequence); store(output.journal_head, 28, 1);
        store(output.journal_head, 252, 0);
        store(output.journal_head, 252, jbd2::detail::crc32c(~u32{0}, output.journal_head));
        const auto new_codec = jbd2::Codec::open(output.journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, 5> allowed{};
        for (usize index = 0; index < output.count; ++index) allowed[index] = {output.targets[index], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks, std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("namespace journal generated record shape"));
        for (usize index = 0; index < replay.actions.size(); ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != output.targets[index] || action.journal_block != index + 2 ||
                action.sequence != output.sequence || action.transaction_order)
                return reject(Status::fault("namespace journal generated target identity"));
            const auto &payload = output.payloads[index];
            for (usize byte = 0; byte < payload.size(); ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ? u8(jbd2::magic >> (24 - byte * 8)) : payload[byte];
                if (restored != canonical.after[index][byte])
                    return reject(Status::fault("namespace journal generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};
} // namespace ok::fs
