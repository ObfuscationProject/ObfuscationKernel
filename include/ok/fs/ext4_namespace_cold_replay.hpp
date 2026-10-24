#pragma once

#include "ok/fs/ext4_namespace_replay.hpp"
#include "ok/fs/ext4_namespace_witness.hpp"

namespace ok::fs
{
// Ancillary bytes and homes must belong to the same immutable, owner-exclusive
// device view. This is a borrowing contract, not device admission authority.
struct Ext4NamespaceColdAncillary
{
    const Ext4NamespaceBlock &superblock;
    const Ext4NamespaceBlock &descriptors;
    const std::array<Ext4NamespaceBlock, 4> &inode_bitmaps;
    const std::array<Ext4NamespaceBlock, 4> &block_bitmaps;
};
struct Ext4NamespaceColdScratch
{
    std::array<Ext4NamespaceBlock, 5> before{}, after{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4NamespaceBlock record{}, payload{}, old_head{};
    Ext4NamespaceJournalScratch canonical_scratch{};
    Ext4NamespaceJournalPlan canonical{};
};
struct Ext4NamespaceColdResult
{
    Ext4NamespaceReplayResult homes{};
    Ext4NamespaceOperation operation{};
    u32 parent{}, source{}, removed_inode{};
    usize witness_runs{}, witness_bytes{};
    bool durable_old_witness{};
};

// Exact preimage reconstruction from the durable descriptor witness, including
// all-after homes. No external historical snapshot or inferred inode times are
// used. This proof does not grant mount, replay I/O or orphan reclaim authority.
class Ext4NamespaceColdReplayVerifier final
{
    using Block = Ext4NamespaceBlock;
    struct Reader
    {
        const Ext4NamespaceReplayRecords &records;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> output)
        {
            if (output.size() != 1024) return Status::invalid_argument("namespace cold record size");
            const Block *source = nullptr;
            if (!number) source = &records.journal_head;
            else if (number == 1) source = &records.descriptor;
            else if (number >= 2 && number < records.payloads.size() + 2)
                source = &records.payloads[number - 2];
            else if (number == records.payloads.size() + 2) source = &records.commit;
            else if (number == records.payloads.size() + 3) source = &records.end;
            if (!source) return Status::io_error("namespace cold record bounds");
            for (usize i = 0; i < output.size(); ++i) output[i] = (*source)[i];
            return {};
        }
    };
    static void big32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (24 - i * 8)); }
    [[nodiscard]] static u32 table_for(const Block &descriptors, u32 inode)
    {
        return ext4_checked::le32(descriptors, ((inode - 1) / 2048) * 32 + 8) +
               ((inode - 1) % 2048) / 4;
    }
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &structural_binding,
        const Ext4NamespaceReplayRecords &records, const Ext4NamespaceColdAncillary &ancillary,
        Ext4NamespaceReplayHomes observed, Ext4NamespaceColdScratch &scratch,
        Ext4NamespaceColdResult &output)
    {
        output = {}; scratch = {};
        if (!structural_binding.needs_recovery || structural_binding.empty_log ||
            structural_binding.layout.block_size != 1024 ||
            structural_binding.layout.journal_first_block < 3 ||
            structural_binding.layout.journal_first_block >= structural_binding.layout.filesystem_blocks ||
            structural_binding.layout.journal_blocks > structural_binding.layout.filesystem_blocks -
                structural_binding.layout.journal_first_block ||
            records.payloads.size() < 2 || records.payloads.size() > 5 ||
            observed.targets.size() != records.payloads.size() ||
            observed.images.size() != observed.targets.size())
            return Status::invalid_argument("namespace cold binding or input sizes");
        const auto codec = jbd2::Codec::open(records.journal_head,
            u32(structural_binding.layout.journal_blocks), structural_binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (!head.sequence || head.sequence != structural_binding.journal_sequence || head.start_block != 1)
            return Status::io_error("namespace cold journal head identity");
        const auto &layout = structural_binding.layout;
        const u64 journal_end = layout.journal_first_block + layout.journal_blocks;
        const std::array<jbd2::BlockRange, 2> allowed{{{0, layout.journal_first_block},
            {journal_end, layout.filesystem_blocks - journal_end}}};
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {layout.filesystem_blocks, std::span<const jbd2::BlockRange>{allowed}.first(
                journal_end < layout.filesystem_blocks ? 2 : 1)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &replay = parsed.value();
        const usize count = records.payloads.size();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.actions.size() != count ||
            replay.next_sequence != head.sequence + 1)
            return Status::io_error("namespace cold journal transaction grammar");
        for (usize i = 0; i < count; ++i)
        {
            const auto &action = replay.actions[i];
            if (action.target_block != observed.targets[i] || action.journal_block != i + 2 ||
                action.sequence != head.sequence || action.transaction_order)
                return Status::io_error("namespace cold home identity or order");
            scratch.after[i] = records.payloads[i];
            if (action.restore_magic) big32(scratch.after[i], 0, jbd2::magic);
        }
        Ext4NamespaceWitness witness{};
        if (auto status = Ext4NamespaceWitnessCodec::decode(records.descriptor,
            std::span<const Block>{scratch.after}.first(count),
            std::span<Block>{scratch.before}.first(count), witness); !status) return status;
        const auto &update = witness.update;
        if (update.parent < 2 || update.parent > 8192 || update.source < 11 || update.source > 8192 ||
            (update.operation == Ext4NamespaceOperation::replace &&
             (update.destination < 11 || update.destination > 8192)))
            return Status::invalid_argument("namespace cold witnessed inode identity");
        const auto find = [&](u32 target) -> const Block * {
            for (usize i = 0; i < count; ++i)
                if (observed.targets[i] == target) return &scratch.before[i];
            return nullptr;
        };
        const u32 parent_block = table_for(ancillary.descriptors, update.parent);
        const u32 source_block = table_for(ancillary.descriptors, update.source);
        const u32 removed = update.operation == Ext4NamespaceOperation::unlink ? update.source : update.destination;
        const u32 destination_block = table_for(ancillary.descriptors, removed);
        const auto *parent = find(parent_block), *source = find(source_block), *destination = find(destination_block);
        const bool has_super = observed.targets[count - 1] == 1;
        const usize directory_at = count - 1 - usize(has_super);
        const u32 directory_block = observed.targets[directory_at];
        const std::array<u32, 1> directories{directory_block};
        const auto shape = Ext4ReplayShapePlanner::classify(structural_binding, {directories}, replay);
        if (!shape) return shape.status();
        if (!shape.value().includes(Ext4ReplayFamily::namespace_change) ||
            !parent || !source || !destination)
            return Status::unsupported("namespace cold home family or table coverage");
        const auto *super = has_super ? find(1) : &ancillary.superblock;
        if (has_super && ancillary.superblock != observed.images[count - 1])
            return Status::io_error("namespace cold ancillary superblock differs from observed home");
        Ext4RecoveryInfo old_binding = structural_binding;
        old_binding.empty_log = true; old_binding.journal_sequence = head.sequence - 1;
        scratch.old_head = records.journal_head;
        big32(scratch.old_head, 24, old_binding.journal_sequence);
        big32(scratch.old_head, 28, 0); big32(scratch.old_head, 252, 0);
        big32(scratch.old_head, 252, jbd2::detail::crc32c(~u32{0}, scratch.old_head));
        // This assertion is checked by the canonical planner against the full
        // witnessed old superblock; it is never an orphan mount bypass.
        const Ext4NamespaceEmptyOrphanAuthorization empty_orphan{old_binding.uuid, 0,
            ext4_checked::le32(*super, 232) == 0};
        const Ext4NamespaceSource source_images{parent_block, source_block, destination_block, directory_block,
            *super, ancillary.descriptors, *parent, *source, *destination, scratch.before[directory_at],
            ancillary.inode_bitmaps, ancillary.block_bitmaps, empty_orphan};
        if (auto status = Ext4NamespaceDurableJournalPlanner::plan(old_binding, update, source_images,
            scratch.old_head, scratch.canonical_scratch, scratch.canonical); !status) return status;
        const auto &expected = scratch.canonical;
        const auto &homes = scratch.canonical_scratch.namespace_plan;
        if (!expected.ready || expected.no_op || expected.count != count || homes.count != count ||
            expected.journal_head != records.journal_head || expected.descriptor != records.descriptor ||
            expected.commit != records.commit || expected.end != records.end)
            return Status::io_error("namespace cold canonical journal bytes differ");
        Ext4NamespaceColdResult verified{};
        verified.operation = update.operation; verified.parent = update.parent;
        verified.source = update.source; verified.removed_inode = removed;
        verified.witness_runs = witness.runs; verified.witness_bytes = witness.bytes;
        verified.durable_old_witness = true;
        verified.homes.count = count; verified.homes.orphan_published = homes.orphan_published;
        verified.homes.allocation_unchanged = homes.allocation_unchanged;
        for (usize i = 0; i < count; ++i)
        {
            if (expected.targets[i] != observed.targets[i] || expected.payloads[i] != records.payloads[i] ||
                homes.before[i] != scratch.before[i] || homes.after[i] != scratch.after[i])
                return Status::io_error("namespace cold canonical payload or witnessed preimage differs");
            verified.homes.targets[i] = observed.targets[i];
            if (observed.images[i] == homes.after[i])
            { verified.homes.home_after[i] = true; ++verified.homes.new_homes; }
            else if (observed.images[i] == homes.before[i]) ++verified.homes.old_homes;
            else return Status::io_error("namespace cold unrelated current home bytes");
        }
        verified.homes.all_before = verified.homes.old_homes == count;
        verified.homes.all_after = verified.homes.new_homes == count;
        output = verified;
        return {};
    }
};
} // namespace ok::fs
