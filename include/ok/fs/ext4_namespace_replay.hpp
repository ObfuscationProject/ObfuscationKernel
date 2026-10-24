#pragma once

#include "ok/fs/ext4_namespace_journal.hpp"
#include "ok/fs/ext4_replay_shape.hpp"

namespace ok::fs
{
// All spans and blocks belong to one caller-protected, immutable snapshot.
// The old source and empty head must be checked independently before this call.
struct Ext4NamespaceReplayRecords
{
    const Ext4NamespaceBlock &journal_head;
    const Ext4NamespaceBlock &descriptor;
    std::span<const Ext4NamespaceBlock> payloads;
    const Ext4NamespaceBlock &commit;
    const Ext4NamespaceBlock &end;
};
struct Ext4NamespaceReplayHomes
{
    std::span<const u32> targets;
    std::span<const Ext4NamespaceBlock> images;
};
struct Ext4NamespaceReplayScratch
{
    Ext4NamespaceJournalScratch canonical_scratch{};
    Ext4NamespaceJournalPlan canonical{};
    Ext4NamespaceBlock record{}, payload{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
};
struct Ext4NamespaceReplayResult
{
    std::array<u32, 5> targets{};
    std::array<bool, 5> home_after{};
    usize count{}, old_homes{}, new_homes{};
    bool all_before{}, all_after{}, orphan_published{}, allocation_unchanged{};
    bool conditional_on_old_witness{};
};

// This proves only the supplied transaction and participant-home snapshot
// relative to a supplied checked old witness. It does not infer unknown old
// inode versions, timestamps, sibling slots or orphan state from final homes.
// No device, replay, checkpoint or mount authority is returned.
class Ext4NamespaceReplayVerifier final
{
    using Block = Ext4NamespaceBlock;
    struct Reader
    {
        const Ext4NamespaceReplayRecords &records;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> output)
        {
            if (output.size() != 1024) return Status::invalid_argument("namespace replay record size");
            const Block *source = nullptr;
            if (number == 0) source = &records.journal_head;
            else if (number == 1) source = &records.descriptor;
            else if (number >= 2 && number < records.payloads.size() + 2)
                source = &records.payloads[number - 2];
            else if (number == records.payloads.size() + 2) source = &records.commit;
            else if (number == records.payloads.size() + 3) source = &records.end;
            if (!source) return Status::io_error("namespace replay record bounds");
            for (usize index = 0; index < output.size(); ++index) output[index] = (*source)[index];
            return {};
        }
    };
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &old_binding,
        const Ext4NamespaceUpdate &update, const Ext4NamespaceSource &checked_old_source,
        const Block &checked_old_empty_head, const Ext4NamespaceReplayRecords &actual_records,
        Ext4NamespaceReplayHomes observed, Ext4NamespaceReplayScratch &scratch,
        Ext4NamespaceReplayResult &result)
    {
        result = {};
        scratch = {};
        if (actual_records.payloads.size() > 5 || observed.targets.size() != observed.images.size() ||
            observed.targets.size() > 5)
            return Status::invalid_argument("namespace replay bounded input sizes");
        const auto status = Ext4NamespaceJournalPlanner::plan(old_binding, update,
            checked_old_source, checked_old_empty_head,
            scratch.canonical_scratch, scratch.canonical);
        if (!status) return status;
        const auto &expected = scratch.canonical;
        const auto &homes = scratch.canonical_scratch.namespace_plan;
        if (expected.no_op || homes.no_op)
            return Status::unsupported("namespace replay has no committed same-inode no-op");
        if (!expected.ready || !expected.count || expected.count != homes.count ||
            actual_records.payloads.size() != expected.count || observed.targets.size() != expected.count)
            return Status::invalid_argument("namespace replay participant count");

        const auto codec = jbd2::Codec::open(actual_records.journal_head,
            u32(old_binding.layout.journal_blocks), old_binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        std::array<jbd2::BlockRange, 5> domain{};
        for (usize index = 0; index < expected.count; ++index)
            domain[index] = {expected.targets[index], 1};
        Reader reader{actual_records};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {old_binding.layout.filesystem_blocks,
             std::span<const jbd2::BlockRange>{domain}.first(expected.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.actions.size() != expected.count ||
            replay.next_sequence != expected.sequence + 1)
            return Status::io_error("namespace replay transaction grammar");
        const std::array<u32, 1> directories{checked_old_source.directory_block};
        const auto shape = Ext4ReplayShapePlanner::classify(old_binding, {directories}, replay);
        if (!shape) return shape.status();
        if (!shape.value().includes(Ext4ReplayFamily::namespace_change))
            return Status::unsupported("namespace replay target shape");
        for (usize index = 0; index < expected.count; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != expected.targets[index] ||
                action.journal_block != index + 2 || action.sequence != expected.sequence ||
                action.transaction_order || observed.targets[index] != expected.targets[index])
                return Status::io_error("namespace replay target order or identity");
        }
        // The generated records have already passed the JBD2 parser. Comparing
        // every supplied byte additionally rejects CRC-correct foreign changes.
        if (actual_records.journal_head != expected.journal_head ||
            actual_records.descriptor != expected.descriptor ||
            actual_records.commit != expected.commit || actual_records.end != expected.end)
            return Status::io_error("namespace replay journal bytes differ");
        for (usize index = 0; index < expected.count; ++index)
            if (actual_records.payloads[index] != expected.payloads[index])
                return Status::io_error("namespace replay payload differs");

        Ext4NamespaceReplayResult verified{};
        verified.count = expected.count;
        verified.orphan_published = homes.orphan_published;
        verified.allocation_unchanged = homes.allocation_unchanged;
        verified.conditional_on_old_witness = true;
        for (usize index = 0; index < expected.count; ++index)
        {
            verified.targets[index] = expected.targets[index];
            const auto &current = observed.images[index];
            if (current == homes.after[index])
            { verified.home_after[index] = true; ++verified.new_homes; }
            else if (current == homes.before[index]) ++verified.old_homes;
            else return Status::io_error("namespace replay participant has unrelated bytes");
        }
        verified.all_before = verified.old_homes == verified.count;
        verified.all_after = verified.new_homes == verified.count;
        result = verified;
        return {};
    }
};
} // namespace ok::fs
