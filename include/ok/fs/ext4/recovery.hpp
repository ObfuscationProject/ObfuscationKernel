#pragma once

// ext4 recovery module: cold replay, recovery bindings and completion of interrupted namespace,
// regular-move and reclaim mutations, their result histories and the orphan-chain
// capture.

#include "ok/fs/ext4/journal.hpp"
#include "ok/fs/ext4/alloc.hpp"
#include "ok/fs/ext4/dir.hpp"
#include "ok/fs/ext4/txn.hpp"
#include "ok/block/guarded_reads.hpp"
#include <type_traits>
#include <new>

namespace ok::fs
{
enum class Ext4ReplayFamily : u8
{
    empty, structural_metadata, absent_rename, empty_create,
    bounded_data_allocation, namespace_change, bounded_directory_create
};
enum class Ext4ReplaySemanticVerdict : u8 { not_this_family, valid, invalid_transition };
struct Ext4ReplaySemanticDecision
{
    Ext4ReplayFamily family{};
    Ext4ReplaySemanticVerdict verdict{};
    Status failure{};
};
struct Ext4ReplayShape
{
    u32 candidates{};
    bool empty_log{};
    [[nodiscard]] constexpr bool includes(Ext4ReplayFamily family) const
    {
        return u8(family) <= u8(Ext4ReplayFamily::bounded_directory_create) &&
               (candidates & (u32{1} << u8(family)));
    }
};

// The caller must independently prove directory ownership in the same stable
// recovery view. This input is not a mount capability or a semantic proof.
struct Ext4ReplayDirectoryDomain
{
    std::span<const u32> checked_blocks{};
};

// Pure home-shape preflight. In particular, a single candidate is not permission
// to replay: exact canonical payload, ownership and session proofs are separate.
class Ext4ReplayShapePlanner final
{
    enum class Role : u8 { other, superblock, descriptors, block_bitmap, inode_bitmap, inode_table, directory };
    [[nodiscard]] static constexpr u32 bit(Ext4ReplayFamily family)
    { return u32{1} << u8(family); }
    [[nodiscard]] static constexpr bool overlaps(jbd2::BlockRange a, jbd2::BlockRange b)
    { return a.first < b.first + b.count && b.first < a.first + a.count; }
    [[nodiscard]] static bool in_journal(const Ext4RecoveryInfo &binding, u64 block)
    {
        return block >= binding.layout.journal_first_block &&
               block - binding.layout.journal_first_block < binding.layout.journal_blocks;
    }
    [[nodiscard]] static bool ordinary_data(const Ext4RecoveryInfo &binding, u64 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks || in_journal(binding, block)) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && block - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static Role role(const Ext4RecoveryInfo &binding,
                                   Ext4ReplayDirectoryDomain directories, u64 block)
    {
        if (block == 1) return Role::superblock;
        if (block == 2) return Role::descriptors;
        for (usize group = 0; group < 4; ++group)
        {
            const usize at = group * 4;
            if (block == binding.metadata[at + 1].first) return Role::block_bitmap;
            if (block == binding.metadata[at + 2].first) return Role::inode_bitmap;
            const auto table = binding.metadata[at + 3];
            if (block >= table.first && block - table.first < table.count) return Role::inode_table;
        }
        for (u32 directory : directories.checked_blocks)
            if (block == directory) return Role::directory;
        return Role::other;
    }
    [[nodiscard]] static Status domain(const Ext4RecoveryInfo &binding,
                                       Ext4ReplayDirectoryDomain directories)
    {
        const auto &layout = binding.layout;
        if (layout.block_size != 1024 || layout.filesystem_blocks != 32768 ||
            binding.metadata_count != 16 || binding.journal_inode != 8 ||
            layout.journal_blocks < 1024 || layout.journal_first_block < 3 ||
            layout.journal_first_block >= layout.filesystem_blocks ||
            layout.journal_blocks > layout.filesystem_blocks - layout.journal_first_block ||
            directories.checked_blocks.size() > 128)
            return Status::unsupported("replay shape bounded domain");
        for (usize group = 0; group < 4; ++group)
        {
            const usize at = group * 4;
            const u64 begin = 1 + group * 8192, end = group == 3 ? 32768 : begin + 8192;
            if (binding.metadata[at].first != begin || binding.metadata[at].count != 2 ||
                binding.metadata[at + 1].count != 1 || binding.metadata[at + 2].count != 1 ||
                binding.metadata[at + 3].count != 512)
                return Status::unsupported("replay shape metadata roles");
            for (usize index = at; index < at + 4; ++index)
            {
                const auto range = binding.metadata[index];
                if (range.first < begin || range.first >= end || !range.count || range.count > end - range.first ||
                    overlaps(range, {layout.journal_first_block, layout.journal_blocks}))
                    return Status::unsupported("replay shape metadata placement");
                for (usize prior = 0; prior < index; ++prior)
                    if (overlaps(range, binding.metadata[prior]))
                        return Status::invalid_argument("replay shape overlapping metadata");
            }
        }
        for (usize index = 0; index < directories.checked_blocks.size(); ++index)
        {
            const u32 block = directories.checked_blocks[index];
            if (block < 3 || block >= layout.filesystem_blocks || in_journal(binding, block))
                return Status::invalid_argument("replay shape directory bounds");
            for (usize prior = 0; prior < index; ++prior)
                if (directories.checked_blocks[prior] == block)
                    return Status::invalid_argument("replay shape duplicate directory ownership");
            for (usize metadata = 0; metadata < binding.metadata_count; ++metadata)
            {
                const auto range = binding.metadata[metadata];
                if (block >= range.first && block - range.first < range.count)
                    return Status::invalid_argument("replay shape directory aliases metadata");
            }
        }
        return {};
    }
  public:
    [[nodiscard]] static Result<Ext4ReplayShape> classify(const Ext4RecoveryInfo &binding,
        Ext4ReplayDirectoryDomain directories, const jbd2::ReplayPlan &plan,
        const std::array<u8, 1024> *directory_witness = nullptr)
    {
        if (const auto status = domain(binding, directories); !status) return status;
        if (plan.empty_log)
        {
            if (!plan.actions.empty() || plan.committed_transactions || plan.revoked_actions || plan.revoke_records || plan.incomplete_tail)
                return Status::invalid_argument("replay shape inconsistent empty plan");
            return Ext4ReplayShape{bit(Ext4ReplayFamily::empty), true};
        }
        if (plan.committed_transactions != 1 || plan.revoked_actions || plan.revoke_records || plan.incomplete_tail ||
            plan.actions.empty() || plan.actions.size() > 8)
            return Status::unsupported("replay shape transaction count or tail");
        const usize count = plan.actions.size();
        const bool directory_marker = directory_witness &&
            Ext4DirectoryCreateWitnessCodec::present(*directory_witness);
        const bool directory_create = (count == 7 || count == 8) && directory_marker &&
            (*directory_witness)[172] == count;
        if ((count > 6 || directory_marker) && !directory_create)
            return Status::unsupported("replay shape directory create requires exact witness marker");
        std::array<Role, 8> roles{};
        for (usize index = 0; index < plan.actions.size(); ++index)
        {
            const auto &action = plan.actions[index];
            if (action.target_block >= binding.layout.filesystem_blocks || in_journal(binding, action.target_block) ||
                action.transaction_order || action.sequence != plan.actions.front().sequence ||
                action.journal_block < 1 || action.journal_block >= binding.layout.journal_blocks)
                return Status::invalid_argument("replay shape action identity");
            for (usize prior = 0; prior < index; ++prior)
                if (plan.actions[prior].target_block == action.target_block ||
                    plan.actions[prior].journal_block == action.journal_block)
                    return Status::invalid_argument("replay shape duplicate action");
            roles[index] = role(binding, directories, action.target_block);
            if (roles[index] == Role::other && !(directory_create && index + 2 == count &&
                ordinary_data(binding, action.target_block)))
                return Status::unsupported("replay shape unowned home");
        }
        u32 candidates = 0;
        if (directory_create && roles[count - 6] == Role::inode_bitmap &&
            roles[count - 5] == Role::descriptors && roles[count - 4] == Role::superblock &&
            roles[count - 3] == Role::block_bitmap &&
            (roles[count - 2] == Role::other || roles[count - 2] == Role::directory) &&
            roles[count - 1] == Role::directory)
        {
            bool tables = true;
            for (usize index = 0; index < count - 6; ++index) tables &= roles[index] == Role::inode_table;
            if (tables) candidates |= bit(Ext4ReplayFamily::bounded_directory_create);
        }
        if (count == 1 && roles[0] == Role::inode_table)
            candidates |= bit(Ext4ReplayFamily::structural_metadata);
        if (count == 4 && roles[0] == Role::block_bitmap && roles[1] == Role::descriptors &&
            roles[2] == Role::superblock && roles[3] == Role::inode_table)
            candidates |= bit(Ext4ReplayFamily::bounded_data_allocation);
        if ((count == 5 || count == 6) && roles[count - 4] == Role::inode_bitmap &&
            roles[count - 3] == Role::descriptors && roles[count - 2] == Role::superblock &&
            roles[count - 1] == Role::directory)
        {
            bool tables = true;
            for (usize index = 0; index < count - 4; ++index) tables &= roles[index] == Role::inode_table;
            if (tables) candidates |= bit(Ext4ReplayFamily::empty_create);
        }
        const bool orphan_home = roles[count - 1] == Role::superblock;
        const usize directory_at = count - 1 - usize(orphan_home);
        if (directory_at >= 1 && directory_at <= 3 && roles[directory_at] == Role::directory)
        {
            bool tables = true;
            for (usize index = 0; index < directory_at; ++index) tables &= roles[index] == Role::inode_table;
            if (tables)
            {
                candidates |= bit(Ext4ReplayFamily::namespace_change);
                if (!orphan_home && directory_at <= 2) candidates |= bit(Ext4ReplayFamily::absent_rename);
            }
        }
        if (!candidates) return Status::unsupported("replay shape unrecognized home order");
        return Ext4ReplayShape{candidates, false};
    }

    // Every decision comes from an operation-specific no-write canonical proof.
    // A rejected recognized transition is terminal; it cannot become fallback.
    [[nodiscard]] static Result<Ext4ReplayFamily> select(Ext4ReplayShape shape,
        std::span<const Ext4ReplaySemanticDecision> decisions)
    {
        constexpr u32 known = (u32{1} << (u8(Ext4ReplayFamily::bounded_directory_create) + 1)) - 1;
        if (!shape.candidates || (shape.candidates & ~known) ||
            shape.empty_log != shape.includes(Ext4ReplayFamily::empty) ||
            (shape.empty_log && shape.candidates != bit(Ext4ReplayFamily::empty)))
            return Status::invalid_argument("replay shape candidate set");
        if (shape.empty_log)
            return decisions.empty() ? Result<Ext4ReplayFamily>{Ext4ReplayFamily::empty} :
                Result<Ext4ReplayFamily>{Status::invalid_argument("replay shape empty decisions")};
        u32 visited = 0, accepted = 0;
        Ext4ReplayFamily selected{};
        for (const auto &decision : decisions)
        {
            if (!shape.includes(decision.family) || (visited & bit(decision.family)))
                return Status::invalid_argument("replay shape duplicate or foreign decision");
            visited |= bit(decision.family);
            if (decision.verdict == Ext4ReplaySemanticVerdict::invalid_transition)
                return !decision.failure ? decision.failure : Status::io_error("replay shape semantic proof rejected");
            if (!decision.failure)
                return Status::invalid_argument("replay shape inconsistent semantic status");
            if (decision.verdict == Ext4ReplaySemanticVerdict::valid)
            { ++accepted; selected = decision.family; }
            else if (decision.verdict != Ext4ReplaySemanticVerdict::not_this_family)
                return Status::invalid_argument("replay shape semantic verdict");
        }
        if (visited != shape.candidates) return Status::invalid_argument("replay shape incomplete semantic decisions");
        if (accepted != 1) return Status::unsupported("replay shape no unique semantic family");
        return selected;
    }
};

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

struct Ext4RegularMoveReplayAncillary
{
    const Ext4RegularMoveBlock &superblock;
    const Ext4RegularMoveBlock &descriptors;
    const std::array<Ext4RegularMoveBlock, 4> &inode_bitmaps;
    const std::array<Ext4RegularMoveBlock, 4> &block_bitmaps;
    const Ext4RegularMoveEmptyOrphanAuthorization &orphan;
};

struct Ext4RegularMoveReplayScratch
{
    std::array<Ext4RegularMoveBlock, Ext4RegularMovePlan::capacity> after{}, before{};
    Ext4RegularMovePlan canonical{};
};

struct Ext4RegularMoveReplayValidation
{
    std::array<bool, Ext4RegularMovePlan::capacity> home_after{};
    usize count{};
    u32 from_parent{}, to_parent{}, source{};
    u32 from_generation{}, to_generation{}, source_generation{};
    bool all_homes_after{};
};

// Pure validation of one already-parsed immutable committed journal snapshot.
// No device reads, writes, cache exclusions, session ownership or classifier.
// The caller supplies raw escaped payloads and homes in replay-action order.
// A successful report is semantic evidence, never recovery I/O authority.
class Ext4RegularMoveReplayValidator final
{
    using Block = Ext4RegularMoveBlock;
    [[nodiscard]] static bool overlap(const void *left, usize left_bytes, const void *right, usize right_bytes)
    {
        if (!left_bytes || !right_bytes) return false;
        const usize a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
        return (a >= b && a - b < right_bytes) || (b >= a && b - a < left_bytes);
    }
    [[nodiscard]] static u32 table_for(const Block &descriptors, u32 number)
    {
        return ext4_checked::le32(descriptors, ((number - 1) / 2048) * 32 + 8) +
               ((number - 1) % 2048) / 4;
    }
    [[nodiscard]] static usize home_for(const Ext4RegularMoveWitness &witness, u32 target)
    {
        for (usize index = 0; index < witness.count; ++index)
            if (witness.targets[index] == target) return index;
        return Ext4RegularMovePlan::capacity;
    }

  public:
    // Capacity/alias refusals preserve both outputs. Later failure leaves a
    // zero report; scratch may contain partial unadmitted reconstructed bytes.
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding, const jbd2::Codec &codec,
        const jbd2::ReplayPlan &replay, const Block &descriptor,
        std::span<const Block> journal_payloads, std::span<const Block> current_homes,
        const Ext4RegularMoveReplayAncillary &ancillary, Ext4RegularMoveReplayScratch &scratch,
        Ext4RegularMoveReplayValidation &output)
    {
        constexpr usize capacity = Ext4RegularMovePlan::capacity;
        if (journal_payloads.size() > capacity || current_homes.size() > capacity || replay.actions.size() > capacity)
            return Status::invalid_argument("regular move replay span capacity");
        const auto aliases = [&](const void *data, usize bytes) {
            return overlap(&scratch, sizeof(scratch), data, bytes) ||
                   overlap(&output, sizeof(output), data, bytes);
        };
        if (overlap(&scratch, sizeof(scratch), &output, sizeof(output)) ||
            aliases(&binding, sizeof(binding)) || aliases(&codec, sizeof(codec)) ||
            aliases(&replay, sizeof(replay)) || aliases(replay.actions.data(), replay.actions.size_bytes()) ||
            aliases(&descriptor, sizeof(descriptor)) ||
            aliases(journal_payloads.data(), journal_payloads.size_bytes()) ||
            aliases(current_homes.data(), current_homes.size_bytes()) ||
            aliases(&ancillary, sizeof(ancillary)) || aliases(&ancillary.superblock, sizeof(Block)) ||
            aliases(&ancillary.descriptors, sizeof(Block)) ||
            aliases(&ancillary.inode_bitmaps, sizeof(ancillary.inode_bitmaps)) ||
            aliases(&ancillary.block_bitmaps, sizeof(ancillary.block_bitmaps)) ||
            aliases(&ancillary.orphan, sizeof(ancillary.orphan)))
            return Status::invalid_argument("regular move replay output aliases input");
        output = {}; scratch = {};
        const usize count = replay.actions.size();
        const auto &head = codec.superblock();
        if (count < 3 || journal_payloads.size() != count || current_homes.size() != count ||
            replay.empty_log || replay.committed_transactions != 1 || replay.incomplete_tail ||
            replay.revoked_actions || replay.revoke_records || head.block_size != 1024 ||
            binding.layout.block_size != 1024 || head.uuid != binding.uuid ||
            head.max_blocks != binding.layout.journal_blocks || head.first_block != 1 || head.start_block != 1 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            count + 3 >= head.max_blocks || head.sequence > 0xfffffffdU ||
            replay.next_sequence != head.sequence + 1 || replay.scanned_blocks != count + 3)
            return Status::unsupported("regular move replay transaction shape");
        if (!Ext4RegularMoveWitnessCodec::present(descriptor))
            return Status::unsupported("regular move replay requires OKMVW001 witness");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (!action.target_block || action.target_block >= binding.layout.filesystem_blocks ||
                action.journal_block != index + 2 || action.transaction_order || action.sequence != head.sequence)
                return Status::unsupported("regular move replay action placement");
            for (usize previous = 0; previous < index; ++previous)
                if (replay.actions[previous].target_block == action.target_block)
                    return Status::unsupported("regular move replay duplicate home");
            const usize at = index ? 44 + (index - 1) * 16 : 12;
            const u32 flags = (action.restore_magic ? jbd2::flag_escape : 0) |
                (index ? jbd2::flag_same_uuid : 0) | (index + 1 == count ? jbd2::flag_last_tag : 0);
            if (jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8))
                return Status::unsupported("regular move replay noncanonical descriptor tags");
        }
        if (!jbd2::detail::all_zero(std::span<const u8>{descriptor}.subspan(28, 16)))
            return Status::unsupported("regular move replay internal descriptor UUID");
        usize tags{};
        if (auto status = codec.descriptor(descriptor, head.sequence, [&](const jbd2::Tag &tag) -> Status {
                if (tags >= count || tag.target_block != replay.actions[tags].target_block ||
                    tag.checksum != replay.actions[tags].checksum ||
                    tag.restore_magic != replay.actions[tags].restore_magic)
                    return Status::io_error("regular move replay descriptor differs from parsed actions");
                ++tags; return {};
            }); !status) return status;
        if (tags != count) return Status::io_error("regular move replay descriptor tag count");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (auto status = codec.payload(journal_payloads[index], head.sequence,
                {action.target_block, action.checksum, action.restore_magic}); !status) return status;
            scratch.after[index] = journal_payloads[index];
            if (action.restore_magic)
            {
                scratch.after[index][0] = 0xc0; scratch.after[index][1] = 0x3b;
                scratch.after[index][2] = 0x39; scratch.after[index][3] = 0x98;
            }
        }
        Ext4RegularMoveWitness witness{};
        if (auto status = Ext4RegularMoveWitnessCodec::decode(descriptor,
            std::span<const Block>{scratch.after}.first(count), std::span<Block>{scratch.before}.first(count), witness);
            !status) return status;
        for (usize index = 0; index < count; ++index)
            if (witness.targets[index] != replay.actions[index].target_block)
                return Status::unsupported("regular move replay witness target differs");
        const auto &update = witness.update;
        const u32 from_table = table_for(ancillary.descriptors, update.from_parent);
        const u32 to_table = table_for(ancillary.descriptors, update.to_parent);
        const u32 source_table = table_for(ancillary.descriptors, update.source);
        const usize from_home = home_for(witness, from_table), to_home = home_for(witness, to_table);
        const usize source_home = home_for(witness, source_table);
        if (from_home == capacity || to_home == capacity || source_home == capacity)
            return Status::unsupported("regular move replay missing witnessed inode table");
        const Ext4RegularMoveSource source{from_table, to_table, source_table, witness.targets[0], witness.targets[1],
            ancillary.superblock, ancillary.descriptors, scratch.before[from_home], scratch.before[to_home],
            scratch.before[source_home], scratch.before[0], scratch.before[1],
            ancillary.inode_bitmaps, ancillary.block_bitmaps, ancillary.orphan};
        if (auto status = Ext4RegularMovePlanner::plan(binding, update, source, scratch.canonical); !status)
            return status;
        if (scratch.canonical.count != count)
            return Status::unsupported("regular move replay canonical home count");
        Ext4RegularMoveReplayValidation result{};
        result.all_homes_after = true;
        for (usize index = 0; index < count; ++index)
        {
            if (scratch.canonical.targets[index] != witness.targets[index] ||
                scratch.canonical.before[index] != scratch.before[index] ||
                scratch.canonical.after[index] != scratch.after[index])
                return Status::unsupported("regular move replay image differs from canonical plan");
            result.home_after[index] = current_homes[index] == scratch.after[index];
            if (!result.home_after[index] && current_homes[index] != scratch.before[index])
                return Status::unsupported("regular move replay home differs from old or new image");
            result.all_homes_after &= result.home_after[index];
        }
        auto canonical_descriptor = descriptor;
        if (auto status = Ext4RegularMoveWitnessCodec::encode(update, scratch.canonical, canonical_descriptor); !status)
            return status;
        if (canonical_descriptor != descriptor)
            return Status::unsupported("regular move replay witness differs from canonical encoding");
        result.count = count; result.from_parent = update.from_parent; result.to_parent = update.to_parent;
        result.source = update.source; result.from_generation = update.from_generation;
        result.to_generation = update.to_generation; result.source_generation = update.source_generation;
        output = result;
        return {};
    }
};

// Actual, read-only cold-device binding for one active OKMVW001 transaction.
// The caller continuously owns quiesced media and excludes caches, replacement
// and other writers. This public source prototype does not acquire that session
// or grant mounted/VFS/capability admission. All retained images are owned here.
class Ext4RegularMoveRecoveryBinding final
{
    friend class Ext4RegularMoveCompletion;
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4RegularMoveBlock;
    static constexpr usize home_capacity = Ext4RegularMovePlan::capacity;
    static constexpr usize journal_capacity = home_capacity + 4;
    static constexpr usize snapshot_capacity = 2 + 8 + 1 + journal_capacity + home_capacity;
    struct Snapshot { u32 block{}; const Block *image{}; };
    struct JournalReader
    {
        const std::array<Block, journal_capacity> &images;
        usize count;
        [[nodiscard]] Status read_block(u32 index, std::span<u8> destination)
        {
            if (index >= count || destination.size() != sizeof(Block))
                return Status::invalid_argument("regular move read outside sealed journal");
            for (usize byte = 0; byte < destination.size(); ++byte) destination[byte] = images[index][byte];
            return {};
        }
    };

  public:
    Ext4RegularMoveRecoveryBinding() = default;
    Ext4RegularMoveRecoveryBinding(const Ext4RegularMoveRecoveryBinding &) = delete;
    Ext4RegularMoveRecoveryBinding &operator=(const Ext4RegularMoveRecoveryBinding &) = delete;
    Ext4RegularMoveRecoveryBinding(Ext4RegularMoveRecoveryBinding &&) = delete;
    Ext4RegularMoveRecoveryBinding &operator=(Ext4RegularMoveRecoveryBinding &&) = delete;

    [[nodiscard]] const Ext4RecoveryInfo *info() const { return admitted_ ? generic_.info() : nullptr; }
    [[nodiscard]] const Ext4RegularMoveReplayValidation *validation() const
    { return admitted_ ? &validation_ : nullptr; }
    [[nodiscard]] const Ext4RegularMovePlan *canonical_plan() const
    { return admitted_ ? &scratch_.canonical : nullptr; }
    [[nodiscard]] usize sealed_snapshot_count() const { return admitted_ ? snapshot_count_ : 0; }

    // One read-only attempt, also supported on read-only devices. No flush,
    // write, family fallback, preimage guessing or mount publication occurs.
    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (attempted_) return Status::already_exists("regular move recovery binding is one-shot");
        attempted_ = true; device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (geometry_.block_size != 512)
            return Status::unsupported("regular move recovery requires 512-byte sectors");
        // Public generic admission owns geometry, head, checksum, allocation and
        // internal-journal extent checks; it never parses a move family.
        if (auto status = generic_.inspect(device, bounds); !status) return status;
        const auto &binding = *generic_.info();
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks != 32768 || binding.journal_inode != 8 ||
            binding.metadata_count != 16 || binding.layout.journal_first_block < 3 ||
            binding.layout.journal_first_block > 0xffffffffULL)
            return Status::unsupported("regular move recovery bounded active profile");
        journal_first_ = u32(binding.layout.journal_first_block);
        if (auto status = observe(1, super_); !status) return status;
        if (auto status = observe(2, descriptors_); !status) return status;
        for (u32 group = 0; group < 4; ++group)
        {
            const usize at = group * 32;
            block_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, at);
            inode_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, at + 4);
            if (auto status = observe(block_bitmap_blocks_[group], block_bitmaps_[group]); !status) return status;
            if (auto status = observe(inode_bitmap_blocks_[group], inode_bitmaps_[group]); !status) return status;
        }
        journal_table_block_ = ext4_checked::le32(descriptors_, 8) + 1; // inode 8, slot 3.
        if (auto status = observe(journal_table_block_, journal_table_); !status) return status;
        if (auto status = journal_identity(binding); !status) return status;
        if (auto status = observe(journal_first_, journal_[0]); !status) return status;
        auto codec = jbd2::Codec::open(journal_[0], binding.layout.journal_blocks, binding.uuid,
                                     jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.sequence != binding.journal_sequence || head.sequence > 0xfffffffdU ||
            head.block_size != 1024 || head.max_blocks != binding.layout.journal_blocks ||
            head.first_block != 1 || head.start_block != 1 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("regular move recovery ring, sequence or feature profile");
        if (auto status = observe(journal_first_ + 1, journal_[1]); !status) return status;
        if (!Ext4RegularMoveWitnessCodec::present(journal_[1]))
            return Status::unsupported("regular move recovery requires explicit OKMVW001 witness");
        usize tags{};
        if (auto status = codec.value().descriptor(journal_[1], head.sequence,
            [&](const jbd2::Tag &) -> Status {
                if (tags == home_capacity) return Status::unsupported("regular move recovery tag capacity");
                ++tags; return {};
            }); !status) return status;
        if (tags < 3 || tags + 3 >= head.max_blocks)
            return Status::unsupported("regular move recovery tag count");
        journal_count_ = tags + 4; // Head, descriptor, payloads, commit, exact zero end.
        for (usize index = 2; index < journal_count_; ++index)
            if (auto status = observe(journal_first_ + u32(index), journal_[index]); !status) return status;
        if (!jbd2::detail::all_zero(journal_[journal_count_ - 1]))
            return Status::unsupported("regular move recovery requires exact zero journal boundary");

        // Directory-data homes lie outside generic structural metadata. This
        // broader bounded read domain grants no replay authority: the strict
        // semantic validator below must authenticate every canonical target.
        std::array<jbd2::BlockRange, 2> ranges{};
        usize range_count{};
        if (journal_first_ > 3) ranges[range_count++] = {3, journal_first_ - 3};
        const u64 journal_end = binding.layout.journal_first_block + binding.layout.journal_blocks;
        if (journal_end < binding.layout.filesystem_blocks)
            ranges[range_count++] = {journal_end, binding.layout.filesystem_blocks - journal_end};
        JournalReader reader{journal_, journal_count_};
        auto replay = jbd2::build_replay_plan(codec.value(), reader,
            jbd2::TargetDomain{binding.layout.filesystem_blocks,
                std::span<const jbd2::BlockRange>{ranges}.first(range_count)},
            jbd2::PlanStorage{record_, payload_, actions_, revocations_});
        if (!replay) return replay.status();
        if (replay.value().actions.size() != tags)
            return Status::unsupported("regular move recovery parsed action count");
        for (usize index = 0; index < tags; ++index)
        {
            if (replay.value().actions[index].target_block > 0xffffffffULL)
                return Status::unsupported("regular move recovery home address width");
            if (auto status = observe(u32(replay.value().actions[index].target_block), homes_[index]); !status) return status;
        }
        Ext4OrphanChainProof proof{};
        const Ext4OrphanChainSource orphan_source{super_, descriptors_, inode_bitmaps_, block_bitmaps_, {}};
        if (auto status = Ext4OrphanChainValidator::validate(binding, orphan_source, proof); !status) return status;
        if (proof.count() || proof.observed_head())
            return Status::unsupported("regular move recovery requires independently empty orphan chain");
        orphan_ = {proof.filesystem_uuid(), proof.observed_head(), true};
        const Ext4RegularMoveReplayAncillary ancillary{super_, descriptors_, inode_bitmaps_, block_bitmaps_, orphan_};
        if (auto status = Ext4RegularMoveReplayValidator::validate(binding, codec.value(), replay.value(), journal_[1],
                std::span<const Block>{journal_}.subspan(2, tags), std::span<const Block>{homes_}.first(tags),
                ancillary, scratch_, validation_); !status) return status;
        empty_head_ = journal_[0];
        store(empty_head_, 24, head.sequence + 2); store(empty_head_, 28, 0); store(empty_head_, 252, 0);
        store(empty_head_, 252, jbd2::detail::crc32c(~u32{0}, empty_head_));
        auto empty = jbd2::Codec::open(empty_head_, binding.layout.journal_blocks, binding.uuid,
                                     jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!empty || empty.value().superblock().start_block ||
            empty.value().superblock().sequence != head.sequence + 2)
            return Status::fault("regular move recovery constructed empty head");
        // Bookend every observed raw block, including ancillary/control blocks,
        // before admitting the immutable semantic report. External exclusivity
        // must continue beyond this check; it is not a concurrency primitive.
        if (auto status = recheck(); !status) return status;
        admitted_ = true;
        return {};
    }

  private:
    static void store(Block &image, usize at, u32 value)
    { for (usize byte = 0; byte < 4; ++byte) image[at + byte] = u8(value >> ((3 - byte) * 8)); }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("regular move recovery geometry changed");
    }
    [[nodiscard]] Status bounded(u32 block) const
    {
        const auto *binding = generic_.info();
        if (!binding || block >= binding->layout.filesystem_blocks ||
            u64{block} * 2 >= bounds_.sector_count || bounds_.sector_count - u64{block} * 2 < 2)
            return Status::invalid_argument("regular move recovery block outside admitted bounds");
        return stable();
    }
    [[nodiscard]] Status read(u32 block, Block &image) const
    {
        if (auto status = bounded(block); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(std::span{image})); !status) return status;
        return stable();
    }
    [[nodiscard]] Status observe(u32 block, Block &image)
    {
        if (auto status = read(block, image); !status) return status;
        for (usize index = 0; index < snapshot_count_; ++index)
            if (snapshots_[index].block == block)
                return *snapshots_[index].image == image ? Status{} :
                    Status::io_error("regular move recovery duplicate snapshot differs");
        if (snapshot_count_ == snapshots_.size()) return Status::no_memory("regular move recovery snapshot capacity");
        snapshots_[snapshot_count_++] = {block, &image};
        return {};
    }
    [[nodiscard]] Status recheck()
    {
        if (auto status = stable(); !status) return status;
        for (usize index = 0; index < snapshot_count_; ++index)
        {
            if (auto status = read(snapshots_[index].block, probe_); !status) return status;
            if (probe_ != *snapshots_[index].image)
                return Status::io_error("regular move recovery exact sealed snapshot changed");
        }
        return stable();
    }
    [[nodiscard]] Status journal_identity(const Ext4RecoveryInfo &binding) const
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{journal_table_}.subspan(3 * 256, 256);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (auto status = inode_checksum(seed, 8, inode); !status) return status;
        if (le16(inode, 0) != 0100600 || le16(inode, 2) || le16(inode, 24) || le16(inode, 26) != 1 ||
            le32(inode, 20) || le32(inode, 32) != 0x80000 || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) || le16(inode, 120) || le16(inode, 122) ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 52) || le16(inode, 58) || le16(inode, 56) != binding.layout.journal_blocks ||
            le32(inode, 60) != binding.layout.journal_first_block ||
            le32(inode, 4) != binding.layout.journal_blocks * 1024 ||
            le32(inode, 28) != binding.layout.journal_blocks * 2)
            return Status::io_error("regular move recovery current journal inode differs from binding");
        return {};
    }

    bool attempted_{}, admitted_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryBinding generic_{};
    Block super_{}, descriptors_{}, journal_table_{}, empty_head_{}, record_{}, payload_{}, probe_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    std::array<Block, journal_capacity> journal_{};
    std::array<Block, home_capacity> homes_{};
    std::array<u32, 4> inode_bitmap_blocks_{}, block_bitmap_blocks_{};
    u32 journal_first_{}, journal_table_block_{};
    usize journal_count_{}, snapshot_count_{};
    std::array<Snapshot, snapshot_capacity> snapshots_{};
    std::array<jbd2::ReplayAction, home_capacity> actions_{};
    std::array<jbd2::Revocation, 1> revocations_{};
    Ext4RegularMoveEmptyOrphanAuthorization orphan_{};
    Ext4RegularMoveReplayScratch scratch_{};
    Ext4RegularMoveReplayValidation validation_{};
};

enum class Ext4RegularMoveCompletionPhase : u8
{
    idle, validating, prepared, rechecking, preflight_flush, homes, home_flush, journal_head,
    journal_flush, verifying, complete, failed,
};

struct Ext4RegularMoveCompletionReport
{
    Ext4RegularMoveCompletionPhase phase{Ext4RegularMoveCompletionPhase::idle};
    Ext4RegularMoveCompletionPhase failed_phase{Ext4RegularMoveCompletionPhase::idle};
    Status status{};
    usize validated_actions{}, sealed_snapshots{}, missing_homes{};
    usize write_attempts{}, completed_writes{}, flush_attempts{}, completed_flushes{};
    u32 active_sequence{}, empty_sequence{};
    bool preflight_durable{}, homes_durable{}, journal_durable{}, physical_verified{}, filesystem_dirty_retained{};
    [[nodiscard]] bool destination_may_have_changed() const { return write_attempts != 0; }
    [[nodiscard]] bool complete() const
    {
        return phase == Ext4RegularMoveCompletionPhase::complete && preflight_durable && homes_durable &&
            journal_durable && physical_verified && filesystem_dirty_retained;
    }
};

// One-shot completion of this distinct active regular-move witness profile.
// The public source-prototype API does not acquire exclusive/quiesced ownership,
// issue a mount capability or admit a mounted transaction. The caller retains
// its exclusive session across preparation, execution and failure retirement.
// Whole-home old/new recovery is supported; torn/foreign images fail closed.
class Ext4RegularMoveCompletion final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4RegularMoveBlock;

  public:
    Ext4RegularMoveCompletion() = default;
    Ext4RegularMoveCompletion(const Ext4RegularMoveCompletion &) = delete;
    Ext4RegularMoveCompletion &operator=(const Ext4RegularMoveCompletion &) = delete;
    Ext4RegularMoveCompletion(Ext4RegularMoveCompletion &&) = delete;
    Ext4RegularMoveCompletion &operator=(Ext4RegularMoveCompletion &&) = delete;
    [[nodiscard]] const Ext4RegularMoveCompletionReport &report() const { return report_; }
    [[nodiscard]] const Ext4RegularMoveRecoveryBinding &binding() const { return binding_; }

    // Preparation performs only actual device reads and semantic admission.
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::idle)
            return Status::already_exists("regular move completion is one-shot");
        report_.phase = Ext4RegularMoveCompletionPhase::validating;
        if (!device.geometry().writable)
            return fail(Status::denied("regular move completion requires writable media"));
        if (auto status = binding_.inspect(device, bounds); !status) return fail(status);
        if (!binding_.geometry_.writable)
            return fail(Status::io_error("regular move completion writable geometry changed"));
        report_.validated_actions = binding_.validation_.count;
        report_.sealed_snapshots = binding_.snapshot_count_;
        report_.active_sequence = binding_.generic_.info()->journal_sequence;
        report_.empty_sequence = report_.active_sequence + 2;
        for (usize index = 0; index < binding_.validation_.count; ++index)
            if (!binding_.validation_.home_after[index]) ++report_.missing_homes;
        report_.phase = Ext4RegularMoveCompletionPhase::prepared;
        return {};
    }

    [[nodiscard]] Status execute()
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared)
            return Status::bad_descriptor("regular move completion is not prepared");
        // Retire readiness before any observation. Even a read/preflight error
        // consumes this attempt, so a stale report cannot be retried in place.
        report_.phase = Ext4RegularMoveCompletionPhase::rechecking;
        if (auto status = binding_.recheck(); !status) return fail(status);
        // Every exact snapshot has been compared before any write or FLUSH.
        // No broad structural/rename profile can substitute for this binding.
        // Establish that the observed active log is durable before any home is
        // written: a successful read may otherwise observe volatile live I/O.
        report_.phase = Ext4RegularMoveCompletionPhase::preflight_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.preflight_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::homes;
        for (usize index = 0; index < binding_.validation_.count; ++index)
            if (!binding_.validation_.home_after[index])
                if (auto status = write(binding_.scratch_.canonical.targets[index],
                                        binding_.scratch_.canonical.after[index]); !status) return fail(status);
        report_.phase = Ext4RegularMoveCompletionPhase::home_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.homes_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::journal_head;
        if (auto status = write(binding_.journal_first_, binding_.empty_head_); !status) return fail(status);
        report_.phase = Ext4RegularMoveCompletionPhase::journal_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.journal_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::verifying;
        if (auto status = verify_physical(); !status) return fail(status);
        report_.physical_verified = true;
        report_.filesystem_dirty_retained = true; // Original dirty superblock is exact and unwritten.
        report_.phase = Ext4RegularMoveCompletionPhase::complete;
        return {};
    }

  private:
    // Only the canonical Owner can bind these complete retained images to its
    // exact cache participants. Public reports remain observations, not seals.
    template <usize Capacity> requires (Capacity >= Ext4RegularMovePlan::capacity + 1)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_)
            return Status::bad_descriptor("regular move logical images lack prepared admission");
        count = binding_.validation_.count + 1;
        blocks[0] = binding_.journal_first_; images[0] = binding_.journal_[0];
        for (usize index = 0; index < binding_.validation_.count; ++index)
        { blocks[index + 1] = binding_.scratch_.canonical.targets[index];
          images[index + 1] = binding_.scratch_.canonical.after[index]; }
        return {};
    }
    [[nodiscard]] const Block *observed_preimage(u64 block) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_) return nullptr;
        for (usize index = 0; index < binding_.snapshot_count_; ++index)
            if (binding_.snapshots_[index].block == block) return binding_.snapshots_[index].image;
        return nullptr;
    }
    template <usize Capacity>
    [[nodiscard]] Status matches(const std::array<u64, Capacity> &blocks,
        const std::array<Block, Capacity> &images, usize count, u32 sequence) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_ ||
            count != binding_.validation_.count + 1 || count > Capacity ||
            sequence != report_.active_sequence || blocks[0] != binding_.journal_first_ ||
            images[0] != binding_.journal_[0])
            return Status::fault("regular move completion original participant identity differs");
        for (usize index = 1; index < count; ++index)
            if (blocks[index] != binding_.scratch_.canonical.targets[index - 1] ||
                images[index] != binding_.scratch_.canonical.after[index - 1])
                return Status::fault("regular move completion original participant image differs");
        return {};
    }
    [[nodiscard]] Status matches_published(const Ext4RegularMoveJournalPlan &journal,
                                            const Ext4RegularMovePlan &plan) const
    {
        const usize count = binding_.validation_.count;
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_ ||
            !journal.ready || !plan.count || count != journal.count || count != plan.count ||
            report_.active_sequence != journal.sequence || binding_.journal_count_ != count + 4 ||
            binding_.journal_[0] != journal.journal_head || binding_.journal_[1] != journal.descriptor ||
            binding_.journal_[count + 2] != journal.commit || binding_.journal_[count + 3] != journal.end)
            return Status::fault("regular move completion differs from original published journal");
        for (usize index = 0; index < count; ++index)
            if (binding_.journal_[index + 2] != journal.payloads[index] ||
                binding_.scratch_.canonical.targets[index] != plan.targets[index] ||
                binding_.scratch_.canonical.before[index] != plan.before[index] ||
                binding_.scratch_.canonical.after[index] != plan.after[index])
                return Status::fault("regular move completion differs from original writer homes");
        return {};
    }
    [[nodiscard]] Status verify_terminal()
    {
        if (!report_.complete()) return Status::bad_descriptor("regular move terminal proof is incomplete");
        return verify_physical();
    }
    [[nodiscard]] Status write(u32 block, const Block &image)
    {
        if (auto status = binding_.bounded(block); !status) return status;
        ++report_.write_attempts;
        if (auto status = binding_.device_->write_blocks(binding_.bounds_.first_sector + u64{block} * 2,
                std::as_bytes(std::span{image})); !status) return status;
        ++report_.completed_writes;
        return binding_.stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = binding_.stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = binding_.device_->flush(); !status) return status;
        ++report_.completed_flushes;
        return binding_.stable();
    }
    [[nodiscard]] Status verify_physical()
    {
        if (auto status = binding_.stable(); !status) return status;
        for (usize snapshot = 0; snapshot < binding_.snapshot_count_; ++snapshot)
        {
            const auto &observed = binding_.snapshots_[snapshot];
            const Block *expected = observed.image;
            if (observed.block == binding_.journal_first_) expected = &binding_.empty_head_;
            else
                for (usize home = 0; home < binding_.validation_.count; ++home)
                    if (observed.block == binding_.scratch_.canonical.targets[home])
                    { expected = &binding_.scratch_.canonical.after[home]; break; }
            // A journal-inode ancillary table may also be a selected home.
            // That one physical block must verify against its whole after image.
            if (auto status = binding_.read(observed.block, binding_.probe_); !status) return status;
            if (binding_.probe_ != *expected)
                return Status::io_error("regular move completion physical image differs");
        }
        return binding_.stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase;
        report_.phase = Ext4RegularMoveCompletionPhase::failed;
        report_.status = status;
        return status;
    }

    Ext4RegularMoveRecoveryBinding binding_{};
    Ext4RegularMoveCompletionReport report_{};
};

enum class Ext4NamespaceRecoveryPhase : u8
{
    idle, bootstrap, journal, before_binding, verifying, ready, failed,
};
struct Ext4NamespaceRecoveryReport
{
    Ext4NamespaceRecoveryPhase phase{Ext4NamespaceRecoveryPhase::idle};
    Status status{};
    usize count{}, old_homes{}, new_homes{};
    u32 active_sequence{}, parsed_next_sequence{}, linux_cleanup_sequence{};
    bool recognized_marker{}, old_binding_validated{}, durable_witness{}, ready{};
};

// One read-only cold admission against an owner-exclusive block session. The
// owner supplies lifetime, epoch, registration and cache exclusion. No device
// write, FLUSH, mount publication or checkpoint authority is provided here.
class Ext4NamespaceRecoveryBinding final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    friend class Ext4NamespaceResultHistory;
    using Block = Ext4NamespaceBlock;
    static constexpr usize capacity = Ext4NamespacePlan::capacity;
    struct JournalReader
    {
        Ext4NamespaceRecoveryBinding &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (number >= owner.journal_blocks_ || bytes.size() != 1024)
                return Status::invalid_argument("namespace recovery journal reader bounds");
            return owner.read_block(owner.journal_first_ + number, bytes);
        }
    };
    class BeforeDevice final : public driver::BlockDevice
    {
      public:
        explicit BeforeDevice(Ext4NamespaceRecoveryBinding &owner) : owner_(owner) {}
        [[nodiscard]] driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        [[nodiscard]] Status read_blocks(u64 sector, std::span<std::byte> bytes) override
        {
            if (auto status = owner_.stable(); !status) return status;
            if (sector < owner_.bounds_.first_sector || bytes.empty() || bytes.size() % 512)
                return Status::invalid_argument("namespace before overlay read bounds");
            const u64 relative = sector - owner_.bounds_.first_sector;
            if (relative >= owner_.bounds_.sector_count ||
                bytes.size() / 512 > owner_.bounds_.sector_count - relative)
                return Status::invalid_argument("namespace before overlay partition bounds");
            if (auto status = owner_.device_->read_blocks(sector, bytes); !status) return status;
            for (usize offset = 0; offset < bytes.size(); offset += 512)
            {
                const u64 within = relative + offset / 512;
                const u64 block = within / 2;
                const Block *image = block == owner_.journal_first_ ? &owner_.old_head_ : nullptr;
                if (!image)
                    for (usize index = 0; index < owner_.count_; ++index)
                        if (owner_.targets_[index] == block) { image = &owner_.before_[index]; break; }
                if (image)
                    for (usize byte = 0; byte < 512; ++byte)
                        bytes[offset + byte] = std::byte((*image)[(within % 2) * 512 + byte]);
            }
            return owner_.stable();
        }
        [[nodiscard]] Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("namespace before overlay is read-only"); }
        [[nodiscard]] Status flush() override
        { return Status::denied("namespace before overlay cannot flush"); }
      private:
        Ext4NamespaceRecoveryBinding &owner_;
    };

  public:
    Ext4NamespaceRecoveryBinding() = default;
    Ext4NamespaceRecoveryBinding(const Ext4NamespaceRecoveryBinding &) = delete;
    Ext4NamespaceRecoveryBinding &operator=(const Ext4NamespaceRecoveryBinding &) = delete;
    Ext4NamespaceRecoveryBinding(Ext4NamespaceRecoveryBinding &&) = delete;
    Ext4NamespaceRecoveryBinding &operator=(Ext4NamespaceRecoveryBinding &&) = delete;
    [[nodiscard]] const Ext4NamespaceRecoveryReport &report() const { return report_; }
    [[nodiscard]] bool ready() const { return report_.ready; }
    [[nodiscard]] const Ext4NamespaceColdResult *proof() const { return ready() ? &proof_ : nullptr; }
    [[nodiscard]] const Ext4NamespaceColdResult *result() const { return proof(); }
    [[nodiscard]] const Ext4NamespaceColdScratch *scratch() const
    { return ready() ? &cold_scratch_ : nullptr; }
    [[nodiscard]] const Ext4RecoveryInfo *info() const
    { return ready() ? &active_info_ : nullptr; }
    [[nodiscard]] const Ext4RecoveryInfo *old_binding() const
    { return ready() ? old_binding_.info() : nullptr; }
    [[nodiscard]] const Block &current_head() const { return head_; }
    [[nodiscard]] const Block &journal_head() const { return head_; }
    [[nodiscard]] const Block &old_head() const { return old_head_; }
    [[nodiscard]] usize count() const { return ready() ? count_ : 0; }
    [[nodiscard]] u32 journal_first() const { return ready() ? journal_first_ : 0; }
    [[nodiscard]] u32 journal_sequence() const { return ready() ? report_.active_sequence : 0; }
    [[nodiscard]] u32 parsed_next_sequence() const { return ready() ? report_.parsed_next_sequence : 0; }
    [[nodiscard]] u32 linux_cleanup_sequence() const { return ready() ? report_.linux_cleanup_sequence : 0; }
    [[nodiscard]] u32 target(usize index) const { return ready() && index < count_ ? targets_[index] : 0; }
    [[nodiscard]] const Block *before(usize index) const
    { return ready() && index < count_ ? &before_[index] : nullptr; }
    [[nodiscard]] const Block *after(usize index) const
    { return ready() && index < count_ ? &after_[index] : nullptr; }
    [[nodiscard]] const Block *current(usize index) const
    { return ready() && index < count_ ? &current_[index] : nullptr; }
    [[nodiscard]] Status read_old_sectors(u64 relative, std::span<std::byte> bytes)
    {
        if (!ready()) return Status::not_initialized("namespace before overlay is unavailable");
        if (relative >= bounds_.sector_count || bytes.empty() || bytes.size() % 512 ||
            bytes.size() / 512 > bounds_.sector_count - relative)
            return Status::invalid_argument("namespace before overlay read range");
        return before_device_.read_blocks(bounds_.first_sector + relative, bytes);
    }
    [[nodiscard]] Status stable() const
    {
        if (!device_) return Status::not_initialized("namespace recovery device absent");
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} :
            Status::io_error("namespace recovery device geometry changed");
    }

    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4NamespaceRecoveryPhase::idle)
            return Status::already_exists("namespace recovery binding is one-shot");
        device_ = &device; geometry_ = device.geometry(); bounds_ = bounds;
        report_.phase = Ext4NamespaceRecoveryPhase::bootstrap;
        if (!geometry_.writable || geometry_.block_size != 512 ||
            bounds.first_sector >= geometry_.block_count || bounds.sector_count < 65536 ||
            bounds.sector_count > geometry_.block_count - bounds.first_sector)
            return fail(Status::invalid_argument("namespace recovery session geometry or bounds"));
        if (auto status = read_block(1, super_); !status) return fail(status);
        if (ext4_checked::le16(super_, 56) != 0xef53 ||
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_}.first(1020)) !=
                ext4_checked::le32(super_, 1020) ||
            ext4_checked::le32(super_, 24) || ext4_checked::le32(super_, 4) != 32768 ||
            ext4_checked::le32(super_, 0) != 8192 || ext4_checked::le32(super_, 20) != 1 ||
            ext4_checked::le32(super_, 32) != 8192 || ext4_checked::le32(super_, 40) != 2048 ||
            ext4_checked::le16(super_, 88) != 256 || ext4_checked::le32(super_, 92) != 4 ||
            ext4_checked::le32(super_, 96) != 0x46 || ext4_checked::le32(super_, 100) != 0x400 ||
            ext4_checked::le32(super_, 224) != 8 || super_[373] != 1)
            return fail(Status::unsupported("namespace recovery bootstrap superblock"));
        for (usize index = 0; index < uuid_.size(); ++index) uuid_[index] = super_[104 + index];
        if (auto status = read_block(2, descriptors_); !status) return fail(status);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid_);
        Block group_copy = descriptors_;
        const u16 expected = ext4_checked::le16(group_copy, 30);
        group_copy[30] = group_copy[31] = 0;
        if (u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
                std::span<const u8>{group_copy}.first(32))) != expected)
            return fail(Status::io_error("namespace recovery group-zero descriptor checksum"));
        const u32 table = ext4_checked::le32(descriptors_, 8);
        if (table < 3 || table >= 32768 - 512)
            return fail(Status::invalid_argument("namespace recovery journal table bounds"));
        if (auto status = read_block(table + 1, table_block_); !status) return fail(status);
        const auto inode = std::span<const u8>{table_block_}.subspan(3 * 256, 256);
        if (auto status = ext4_checked::inode_checksum(seed, 8, inode); !status) return fail(status);
        journal_blocks_ = ext4_checked::le16(inode, 56);
        journal_first_ = ext4_checked::le32(inode, 60);
        if (ext4_checked::le16(inode, 0) != 0100600 || ext4_checked::le16(inode, 26) != 1 ||
            ext4_checked::le32(inode, 20) || ext4_checked::le32(inode, 32) != 0x80000 ||
            ext4_checked::le16(inode, 40) != 0xf30a || ext4_checked::le16(inode, 42) != 1 ||
            ext4_checked::le16(inode, 44) != 4 || ext4_checked::le16(inode, 46) ||
            ext4_checked::le32(inode, 52) || ext4_checked::le16(inode, 58) ||
            journal_blocks_ < 1024 || journal_blocks_ > 32768 ||
            journal_first_ < 3 || journal_first_ >= 32768 ||
            journal_blocks_ > 32768 - journal_first_ ||
            ext4_checked::le32(inode, 4) != journal_blocks_ * 1024 ||
            ext4_checked::le32(inode, 28) != journal_blocks_ * 2)
            return fail(Status::unsupported("namespace recovery contiguous journal inode"));
        report_.phase = Ext4NamespaceRecoveryPhase::journal;
        if (auto status = read_block(journal_first_, head_); !status) return fail(status);
        const auto codec = jbd2::Codec::open(head_, journal_blocks_, uuid_,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return fail(codec.status());
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.max_blocks != journal_blocks_ ||
            journal.first_block != 1 || journal.start_block != 1 ||
            journal.sequence < 2 || journal.sequence > 0xfffffffdU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return fail(Status::unsupported("namespace recovery active journal head profile"));
        report_.active_sequence = journal.sequence;
        if (auto status = read_block(journal_first_ + 1, descriptor_); !status) return fail(status);
        if (!Ext4NamespaceWitnessCodec::present(descriptor_))
            return fail(Status::unsupported("namespace recovery durable descriptor marker absent"));
        report_.recognized_marker = true;
        if (auto status = codec.value().descriptor(descriptor_, journal.sequence,
                [&](const jbd2::Tag &tag) -> Status {
                    if (count_ == capacity || tag.target_block > 0xffffffffULL ||
                        tag.target_block >= 32768 ||
                        (tag.target_block >= journal_first_ &&
                         tag.target_block - journal_first_ < journal_blocks_))
                        return Status::invalid_argument("namespace recovery target capacity or bounds");
                    for (usize prior = 0; prior < count_; ++prior)
                        if (targets_[prior] == tag.target_block)
                            return Status::io_error("namespace recovery duplicate home target");
                    targets_[count_++] = u32(tag.target_block);
                    return {};
                }); !status) return fail(status);
        if (count_ < 2 || count_ + 3 >= journal_blocks_)
            return fail(Status::unsupported("namespace recovery action count or ring"));
        for (usize index = 0; index < count_; ++index)
            if (auto status = read_block(journal_first_ + u32(index) + 2, payloads_[index]); !status)
                return fail(status);
        if (auto status = read_block(journal_first_ + u32(count_) + 2, commit_); !status)
            return fail(status);
        if (auto status = read_block(journal_first_ + u32(count_) + 3, end_); !status)
            return fail(status);
        const u64 journal_end = u64{journal_first_} + journal_blocks_;
        const std::array<jbd2::BlockRange, 2> allowed{{{0, journal_first_},
            {journal_end, 32768 - journal_end}}};
        JournalReader reader{*this};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {32768, std::span<const jbd2::BlockRange>{allowed}.first(journal_end < 32768 ? 2 : 1)},
            {record_scratch_, payload_scratch_, actions_, revocations_});
        if (!parsed) return fail(parsed.status());
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.actions.size() != count_ ||
            replay.next_sequence != journal.sequence + 1 || !jbd2::detail::all_zero(end_))
            return fail(Status::io_error("namespace recovery journal transaction grammar"));
        for (usize index = 0; index < count_; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != targets_[index] || action.journal_block != index + 2 ||
                action.sequence != journal.sequence || action.transaction_order)
                return fail(Status::io_error("namespace recovery journal action order"));
            after_[index] = payloads_[index];
            if (action.restore_magic)
                for (usize byte = 0; byte < 4; ++byte)
                    after_[index][byte] = u8(jbd2::magic >> (24 - byte * 8));
        }
        Ext4NamespaceWitness witness{};
        if (auto status = Ext4NamespaceWitnessCodec::decode(descriptor_,
                std::span<const Block>{after_}.first(count_),
                std::span<Block>{before_}.first(count_), witness); !status) return fail(status);
        old_head_ = head_;
        big32(old_head_, 24, journal.sequence - 1);
        big32(old_head_, 28, 0); big32(old_head_, 252, 0);
        big32(old_head_, 252, jbd2::detail::crc32c(~u32{0}, old_head_));

        report_.phase = Ext4NamespaceRecoveryPhase::before_binding;
        if (auto status = old_binding_.inspect(before_device_, bounds); !status) return fail(status);
        const auto &old = *old_binding_.info();
        if (!old.needs_recovery || !old.empty_log || old.journal_sequence != journal.sequence - 1 ||
            old.uuid != uuid_ || old.layout.first_sector != bounds.first_sector ||
            old.layout.block_size != 1024 || old.layout.filesystem_blocks != 32768 ||
            old.layout.journal_first_block != journal_first_ || old.layout.journal_blocks != journal_blocks_)
            return fail(Status::io_error("namespace recovery before binding differs from bootstrap"));
        report_.old_binding_validated = true;
        report_.phase = Ext4NamespaceRecoveryPhase::verifying;
        for (u32 group = 0; group < 4; ++group)
        {
            const usize offset = group * 32;
            if (auto status = read_block(ext4_checked::le32(descriptors_, offset),
                    block_bitmaps_[group]); !status) return fail(status);
            if (auto status = read_block(ext4_checked::le32(descriptors_, offset + 4),
                    inode_bitmaps_[group]); !status) return fail(status);
        }
        for (usize index = 0; index < count_; ++index)
            if (auto status = read_block(targets_[index], current_[index]); !status) return fail(status);
        Ext4RecoveryInfo active = old;
        active.empty_log = false; active.journal_sequence = journal.sequence;
        const Ext4NamespaceReplayRecords records{head_, descriptor_,
            std::span<const Block>{payloads_}.first(count_), commit_, end_};
        const Ext4NamespaceColdAncillary ancillary{super_, descriptors_, inode_bitmaps_, block_bitmaps_};
        const Ext4NamespaceReplayHomes homes{std::span<const u32>{targets_}.first(count_),
            std::span<const Block>{current_}.first(count_)};
        if (auto status = Ext4NamespaceColdReplayVerifier::verify(active, records, ancillary,
                homes, cold_scratch_, proof_); !status) return fail(status);
        if (!proof_.durable_old_witness || proof_.homes.count != count_ ||
            cold_scratch_.old_head != old_head_ ||
            cold_scratch_.canonical.journal_head != head_ ||
            cold_scratch_.canonical.count != count_)
            return fail(Status::io_error("namespace recovery before overlay is incomplete"));
        for (usize index = 0; index < count_; ++index)
            if (targets_[index] != cold_scratch_.canonical.targets[index] ||
                before_[index] != cold_scratch_.before[index] ||
                after_[index] != cold_scratch_.after[index])
                return fail(Status::io_error("namespace recovery canonical overlay differs"));
        if (auto status = stable(); !status) return fail(status);
        report_.count = count_; report_.old_homes = proof_.homes.old_homes;
        report_.new_homes = proof_.homes.new_homes;
        report_.parsed_next_sequence = replay.next_sequence;
        report_.linux_cleanup_sequence = replay.next_sequence + 1;
        active_info_ = active;
        report_.durable_witness = true; report_.ready = true;
        report_.phase = Ext4NamespaceRecoveryPhase::ready;
        return {};
    }
    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    { return prepare(device, bounds); }

  private:
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] Status read_block(u32 block, std::span<u8> image) const
    {
        if (block >= 32768 || image.size() != 1024 || u64{block} * 2 + 2 > bounds_.sector_count)
            return Status::invalid_argument("namespace recovery block read bounds");
        if (auto status = stable(); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(image)); !status) return status;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.phase = Ext4NamespaceRecoveryPhase::failed;
        report_.status = status; report_.ready = false;
        proof_ = {};
        return status;
    }

    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    BeforeDevice before_device_{*this};
    Ext4RecoveryBinding old_binding_{};
    Ext4NamespaceRecoveryReport report_{};
    Ext4NamespaceColdScratch cold_scratch_{};
    Ext4NamespaceColdResult proof_{};
    Ext4RecoveryInfo active_info_{};
    std::array<u8, 16> uuid_{};
    u32 journal_first_{}, journal_blocks_{};
    usize count_{};
    std::array<u32, capacity> targets_{};
    std::array<Block, capacity> payloads_{}, before_{}, after_{}, current_{};
    Block super_{}, descriptors_{}, table_block_{}, head_{}, old_head_{}, descriptor_{}, commit_{}, end_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    Block record_scratch_{}, payload_scratch_{};
    std::array<jbd2::ReplayAction, capacity> actions_{};
    std::array<jbd2::Revocation, 1> revocations_{};
};
using Ext4NamespaceRecoveryAdmission = Ext4NamespaceRecoveryBinding;

// Only a canonical owner may execute this checked, same-session proof. An empty
// journal with a published orphan is a cleanup state, not ordinary mount access.
class Ext4NamespaceCompletion final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4NamespaceBlock;
  public:
    Ext4NamespaceCompletion() = default;
    Ext4NamespaceCompletion(const Ext4NamespaceCompletion &) = delete;
    Ext4NamespaceCompletion &operator=(const Ext4NamespaceCompletion &) = delete;
    [[nodiscard]] const Ext4CompletionReport &report() const { return report_; }
    [[nodiscard]] bool orphan_cleanup_pending() const { return orphan_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4CompletionPhase::idle)
            return Status::already_exists("namespace completion is one-shot");
        report_.phase = Ext4CompletionPhase::validating;
        device_ = &device; geometry_ = device.geometry();
        if (!geometry_.writable || geometry_.block_size != 512)
            return fail(Status::unsupported("namespace completion requires writable 512-byte sectors"));
        if (auto status = admission_.inspect(device, bounds); !status) return fail(status);
        const auto *info = admission_.info();
        const auto &proof = *admission_.result();
        if (!info || !proof.durable_old_witness || !proof.homes.allocation_unchanged ||
            proof.homes.count < 2 || proof.homes.count > 5 || info->journal_sequence > 0xfffffffeU)
            return fail(Status::fault("namespace completion admitted proof profile"));
        layout_ = info->layout; count_ = proof.homes.count;
        sequence_ = info->journal_sequence; orphan_ = proof.homes.orphan_published;
        report_.validated_actions = count_;
        // Keep the existing completion convention: reserve the parser's next
        // sequence and store the following empty-head sequence. Offline Linux
        // recovery also maps active N to empty N+2 in the qualified profile.
        report_.journal_sequence = sequence_ + 2;
        journal_ = admission_.journal_head();
        big32(journal_, 24, report_.journal_sequence); big32(journal_, 28, 0);
        big32(journal_, 252, 0);
        big32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        report_.phase = Ext4CompletionPhase::preflight;
        return {};
    }
    template <usize Capacity> requires (Capacity >= 6)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
                                        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4CompletionPhase::preflight)
            return Status::bad_descriptor("namespace completion has no admitted images");
        blocks[0] = layout_.journal_first_block; images[0] = admission_.journal_head();
        for (usize i = 0; i < count_; ++i)
        { blocks[i + 1] = admission_.result()->homes.targets[i]; images[i + 1] = admission_.scratch()->after[i]; }
        count = count_ + 1;
        return {};
    }
    template <usize Capacity>
    [[nodiscard]] Status matches(const std::array<u64, Capacity> &blocks,
                                 const std::array<Block, Capacity> &images, usize count,
                                 u32 sequence) const
    {
        if (report_.phase != Ext4CompletionPhase::preflight || sequence != sequence_ ||
            count != count_ + 1 || count > Capacity || blocks[0] != layout_.journal_first_block ||
            images[0] != admission_.journal_head())
            return Status::fault("namespace completion publication identity");
        for (usize i = 0; i < count_; ++i)
            if (blocks[i + 1] != admission_.result()->homes.targets[i] ||
                images[i + 1] != admission_.scratch()->after[i])
                return Status::fault("namespace completion canonical publication bytes differ");
        return {};
    }
    [[nodiscard]] Status execute()
    {
        if (report_.phase != Ext4CompletionPhase::preflight)
            return Status::bad_descriptor("namespace completion is not prepared");
        // Cache staging may have performed reads since admission. Recheck the
        // complete transaction and whole observed homes before any transport
        // ordering change. This does not refresh or weaken the sealed proof.
        if (auto status = current(); !status) return fail(status);
        if (auto status = flush(); !status) return fail(status);
        const auto &homes = admission_.result()->homes;
        if (!homes.all_after)
        {
            report_.phase = Ext4CompletionPhase::replaying;
            for (usize i = 0; i < count_; ++i)
                if (!homes.home_after[i])
                    if (auto status = write(homes.targets[i], admission_.scratch()->after[i]); !status)
                        return fail(status);
            report_.phase = Ext4CompletionPhase::home_flush;
            if (auto status = flush(); !status) return fail(status);
        }
        report_.home_durable = true;
        report_.phase = Ext4CompletionPhase::journal_clean;
        if (auto status = write(u32(layout_.journal_first_block), journal_); !status) return fail(status);
        report_.phase = Ext4CompletionPhase::journal_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.journal_durable = true;
        // The recovery bit and allocated orphan are deliberately retained.
        // Compare physical bytes; a successful transport status is insufficient.
        for (usize i = 0; i < count_; ++i)
            if (auto status = equal(homes.targets[i], admission_.scratch()->after[i]); !status)
                return fail(status);
        if (auto status = equal(u32(layout_.journal_first_block), journal_); !status) return fail(status);
        report_.filesystem_durable = true;
        report_.phase = Ext4CompletionPhase::complete;
        return {};
    }
    [[nodiscard]] Status matches_published(const Ext4NamespaceJournalPlan &journal,
                                           const Ext4NamespacePlan &homes) const
    {
        const auto &canonical = admission_.scratch()->canonical;
        if (report_.phase != Ext4CompletionPhase::preflight || !journal.ready || journal.no_op ||
            homes.no_op || journal.sequence != sequence_ || journal.count != count_ || homes.count != count_ ||
            journal.journal_head != admission_.journal_head() || journal.descriptor != canonical.descriptor ||
            journal.commit != canonical.commit || journal.end != canonical.end)
            return Status::fault("namespace completion differs from owned durable journal");
        for (usize i = 0; i < count_; ++i)
            if (journal.targets[i] != canonical.targets[i] || homes.targets[i] != canonical.targets[i] ||
                journal.payloads[i] != canonical.payloads[i] || homes.before[i] != admission_.scratch()->before[i] ||
                homes.after[i] != admission_.scratch()->after[i])
                return Status::fault("namespace completion differs from owned historical images");
        return {};
    }
    [[nodiscard]] Status current()
    {
        const auto &canonical = admission_.scratch()->canonical;
        const u32 first = u32(layout_.journal_first_block);
        if (auto status = equal(first, admission_.journal_head()); !status) return status;
        if (auto status = equal(first + 1, canonical.descriptor); !status) return status;
        for (usize i = 0; i < count_; ++i)
        {
            if (auto status = equal(first + u32(i) + 2, canonical.payloads[i]); !status) return status;
            const auto &observed = admission_.result()->homes.home_after[i] ?
                admission_.scratch()->after[i] : admission_.scratch()->before[i];
            if (auto status = equal(admission_.result()->homes.targets[i], observed); !status) return status;
        }
        if (auto status = equal(first + u32(count_) + 2, canonical.commit); !status) return status;
        return equal(first + u32(count_) + 3, canonical.end);
    }
    [[nodiscard]] Status equal(u32 block, const Block &expected)
    {
        if (auto status = stable(); !status) return status;
        if (block >= layout_.filesystem_blocks) return Status::invalid_argument("namespace completion read bounds");
        if (auto status = device_->read_blocks(layout_.first_sector + u64{block} * 2,
            std::as_writable_bytes(std::span{payload_})); !status) return status;
        if (auto status = stable(); !status) return status;
        return payload_ == expected ? Status{} : Status::io_error("namespace completion physical image differs");
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("namespace completion device changed");
    }
    [[nodiscard]] Status write(u32 block, const Block &image)
    {
        if (block >= layout_.filesystem_blocks) return Status::invalid_argument("namespace completion write bounds");
        if (auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (auto status = device_->write_blocks(layout_.first_sector + u64{block} * 2,
            std::as_bytes(std::span{image})); !status) return status;
        ++report_.completed_writes;
        return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = device_->flush(); !status) return status;
        ++report_.completed_flushes;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase; report_.phase = Ext4CompletionPhase::failed;
        report_.status = status; return status;
    }
    static void big32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (24 - i * 8)); }
    Ext4NamespaceRecoveryAdmission admission_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    jbd2::RecoveryLayout layout_{};
    Ext4CompletionReport report_{};
    Block journal_{}, payload_{};
    usize count_{};
    u32 sequence_{};
    bool orphan_{};
};

// Private prospective bytes. This seal never authorizes transport writes.
class Ext4NamespaceResultHistory final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4OrphanChainCapture;
    using Block = Ext4NamespaceBlock;
  public:
    Ext4NamespaceResultHistory() = default;
    Ext4NamespaceResultHistory(const Ext4NamespaceResultHistory &) = delete;
    Ext4NamespaceResultHistory &operator=(const Ext4NamespaceResultHistory &) = delete;
    Ext4NamespaceResultHistory(Ext4NamespaceResultHistory &&) = delete;
    Ext4NamespaceResultHistory &operator=(Ext4NamespaceResultHistory &&) = delete;
  private:
    static bool same_info(const Ext4RecoveryInfo &a, const Ext4RecoveryInfo &b)
    {
        const auto &x = a.layout, &y = b.layout;
        if (x.first_sector != y.first_sector || x.filesystem_blocks != y.filesystem_blocks ||
            x.journal_first_block != y.journal_first_block || x.journal_blocks != y.journal_blocks ||
            x.block_size != y.block_size || a.uuid != b.uuid || a.metadata_count != b.metadata_count ||
            a.metadata_count > a.metadata.size() || a.journal_inode != b.journal_inode ||
            a.journal_sequence != b.journal_sequence || a.needs_recovery != b.needs_recovery ||
            a.empty_log != b.empty_log) return false;
        for (usize index = 0; index < a.metadata.size(); ++index)
            if (a.metadata[index].first != b.metadata[index].first ||
                a.metadata[index].count != b.metadata[index].count) return false;
        return true;
    }
    static bool same_proof(const Ext4NamespaceColdResult &a, const Ext4NamespaceColdResult &b)
    {
        const auto &x = a.homes, &y = b.homes;
        return a.operation == b.operation && a.parent == b.parent && a.source == b.source &&
            a.removed_inode == b.removed_inode && a.witness_runs == b.witness_runs &&
            a.witness_bytes == b.witness_bytes && a.durable_old_witness == b.durable_old_witness &&
            x.targets == y.targets && x.home_after == y.home_after && x.count == y.count &&
            x.old_homes == y.old_homes && x.new_homes == y.new_homes && x.all_before == y.all_before &&
            x.all_after == y.all_after && x.orphan_published == y.orphan_published &&
            x.allocation_unchanged == y.allocation_unchanged &&
            x.conditional_on_old_witness == y.conditional_on_old_witness;
    }
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] Status prepare(const Ext4NamespaceRecoveryBinding &binding)
    {
        if (binding_ || ready_) return Status::already_exists("namespace result history is one-shot");
        binding_ = &binding;
        if (!binding.ready() || !binding.info() || !binding.old_binding() || !binding.result() ||
            !binding.result()->durable_old_witness || !binding.result()->homes.orphan_published ||
            !binding.result()->homes.allocation_unchanged || binding.count() < 2 || binding.count() > 5 ||
            binding.parsed_next_sequence() == ~u32{0} ||
            binding.parsed_next_sequence() != binding.journal_sequence() + 1 ||
            binding.linux_cleanup_sequence() != binding.parsed_next_sequence() + 1 ||
            binding.linux_cleanup_sequence() > ~u32{0} - 3)
            return Status::unsupported("namespace prospective single-orphan result profile");
        old_info_ = *binding.old_binding(); active_info_ = *binding.info(); proof_ = *binding.result();
        count_ = binding.count_; targets_ = binding.targets_; current_ = binding.current_;
        before_ = binding.before_; after_ = binding.after_; payloads_ = binding.payloads_;
        super_ = binding.super_; descriptors_ = binding.descriptors_; table_ = binding.table_block_;
        inode_bitmaps_ = binding.inode_bitmaps_; block_bitmaps_ = binding.block_bitmaps_;
        head_ = binding.head_; old_head_ = binding.old_head_; descriptor_ = binding.descriptor_;
        commit_ = binding.commit_; end_ = binding.end_; bounds_ = binding.bounds_;
        geometry_ = binding.geometry_; device_ = binding.device_;
        journal_first_ = binding.journal_first_; journal_blocks_ = binding.journal_blocks_;
        sequence_ = binding.journal_sequence(); cleanup_sequence_ = binding.linux_cleanup_sequence();
        const Block *old_super{}, *new_super{}, *current_super{};
        for (usize index = 0; index < count_; ++index)
            if (targets_[index] == 1) { old_super = &before_[index]; new_super = &after_[index]; current_super = &current_[index]; }
        if (!old_super || !new_super || ext4_checked::le32(*old_super, 232) ||
            !current_super || *current_super != super_ ||
            ext4_checked::le32(*new_super, 232) != proof_.removed_inode ||
            proof_.removed_inode < 11 || proof_.removed_inode > 8192 ||
            old_info_.uuid != active_info_.uuid || !old_info_.empty_log || active_info_.empty_log ||
            !old_info_.needs_recovery || !active_info_.needs_recovery ||
            old_info_.journal_sequence != sequence_ - 1 || active_info_.journal_sequence != sequence_)
            return Status::fault("namespace prospective old or resulting head differs");
        empty_head_ = head_; big32(empty_head_, 24, cleanup_sequence_); big32(empty_head_, 28, 0);
        big32(empty_head_, 252, 0);
        big32(empty_head_, 252, jbd2::detail::crc32c(~u32{0}, empty_head_));
        const auto codec = jbd2::Codec::open(empty_head_, journal_blocks_, active_info_.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec || codec.value().superblock().sequence != cleanup_sequence_ ||
            codec.value().superblock().start_block)
            return Status::fault("namespace prospective empty head checksum differs");
        ready_ = true; return stable();
    }
    [[nodiscard]] Status stable() const
    {
        if (!ready_ || !binding_ || !binding_->ready() || binding_->device_ != device_ ||
            !binding_->old_binding() || !binding_->info() || !binding_->result() ||
            binding_->count_ != count_ || binding_->targets_ != targets_ || binding_->current_ != current_ ||
            binding_->before_ != before_ || binding_->after_ != after_ || binding_->payloads_ != payloads_ ||
            binding_->super_ != super_ || binding_->descriptors_ != descriptors_ || binding_->table_block_ != table_ ||
            binding_->inode_bitmaps_ != inode_bitmaps_ || binding_->block_bitmaps_ != block_bitmaps_ ||
            binding_->head_ != head_ || binding_->old_head_ != old_head_ || binding_->descriptor_ != descriptor_ ||
            binding_->commit_ != commit_ || binding_->end_ != end_ || binding_->journal_first_ != journal_first_ ||
            binding_->journal_blocks_ != journal_blocks_ || binding_->bounds_.first_sector != bounds_.first_sector ||
            binding_->bounds_.sector_count != bounds_.sector_count || binding_->journal_sequence() != sequence_ ||
            binding_->parsed_next_sequence() != sequence_ + 1 || binding_->linux_cleanup_sequence() != cleanup_sequence_ ||
            !same_info(*binding_->old_binding(), old_info_) || !same_info(*binding_->info(), active_info_) ||
            !same_proof(*binding_->result(), proof_))
            return Status::fault("namespace prospective sealed proof changed");
        const auto &canonical = binding_->cold_scratch_.canonical;
        if (!canonical.ready || canonical.no_op || canonical.count != count_ || canonical.sequence != sequence_ ||
            canonical.targets != targets_ || canonical.journal_head != head_ || canonical.descriptor != descriptor_ ||
            canonical.payloads != payloads_ || canonical.commit != commit_ || canonical.end != end_ ||
            binding_->cold_scratch_.before != before_ || binding_->cold_scratch_.after != after_ ||
            binding_->cold_scratch_.old_head != old_head_)
            return Status::fault("namespace prospective canonical proof changed");
        Block expected_head = head_;
        big32(expected_head, 24, cleanup_sequence_); big32(expected_head, 28, 0); big32(expected_head, 252, 0);
        big32(expected_head, 252, jbd2::detail::crc32c(~u32{0}, expected_head));
        if (empty_head_ != expected_head)
            return Status::fault("namespace prospective derived whole empty head changed");
        const auto geometry = device_->geometry();
        if (geometry.block_size != geometry_.block_size || geometry.block_count != geometry_.block_count ||
            geometry.writable != geometry_.writable || binding_->geometry_.block_size != geometry_.block_size ||
            binding_->geometry_.block_count != geometry_.block_count || binding_->geometry_.writable != geometry_.writable)
            return Status::io_error("namespace prospective original geometry changed");
        return {};
    }
    [[nodiscard]] const Block *physical(u32 block) const
    {
        if (block == 1) return &super_;
        if (block == 2) return &descriptors_;
        if (block == ext4_checked::le32(descriptors_, 8) + 1) return &table_;
        for (usize group = 0; group < 4; ++group)
        {
            if (block == ext4_checked::le32(descriptors_, group * 32)) return &block_bitmaps_[group];
            if (block == ext4_checked::le32(descriptors_, group * 32 + 4)) return &inode_bitmaps_[group];
        }
        if (block == journal_first_) return &head_;
        if (block == journal_first_ + 1) return &descriptor_;
        for (usize index = 0; index < count_; ++index)
        {
            if (block == targets_[index]) return &current_[index];
            if (block == journal_first_ + u32(index) + 2) return &payloads_[index];
        }
        if (block == journal_first_ + u32(count_) + 2) return &commit_;
        if (block == journal_first_ + u32(count_) + 3) return &end_;
        return nullptr;
    }
    [[nodiscard]] Status substitute(u32 block, std::span<std::byte> bytes) const
    {
        if (auto status = stable(); !status) return status;
        if (bytes.size() != 1024) return Status::invalid_argument("namespace prospective block width");
        const Block *current = physical(block), *result{};
        if (current)
            for (usize byte = 0; byte < 1024; ++byte)
                if (bytes[byte] != std::byte((*current)[byte]))
                    return Status::io_error("namespace prospective whole physical source changed");
        if (block == journal_first_) result = &empty_head_;
        else for (usize index = 0; index < count_; ++index)
            if (block == targets_[index]) result = &after_[index];
        if (result)
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = std::byte((*result)[byte]);
        return stable();
    }
    const Ext4NamespaceRecoveryBinding *binding_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryInfo old_info_{}, active_info_{};
    Ext4NamespaceColdResult proof_{};
    std::array<u32, 5> targets_{};
    std::array<Block, 5> current_{}, before_{}, after_{}, payloads_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    Block super_{}, descriptors_{}, table_{}, head_{}, old_head_{}, empty_head_{}, descriptor_{}, commit_{}, end_{};
    usize count_{};
    u32 journal_first_{}, journal_blocks_{}, sequence_{}, cleanup_sequence_{};
    bool ready_{};
};

// Exclusive caller-owned storage. Never allocate either storage or semantic
// scratch on a kernel stack. They must remain disjoint from all other users.
struct Ext4OrphanReclaimCaptureStorage
{
    Ext4OrphanBlock superblock{}, descriptors{}, head{}, descriptor{}, commit{}, end{};
    std::array<Ext4OrphanBlock, 5> payloads{};
    std::array<Ext4OrphanBlock, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<std::array<Ext4OrphanBlock, 512>, 4> tables{};
    std::array<Ext4OrphanDirectoryBlockImage,
        Ext4OrphanDirectoryReferenceScratch::block_capacity> directories{};
};

enum class Ext4OrphanReclaimCapturePhase : u8 { unused, reading, admitted, checking, rejected };
struct Ext4OrphanReclaimCaptureReport
{
    Ext4OrphanReclaimCapturePhase phase{Ext4OrphanReclaimCapturePhase::unused};
    Status status{};
    block::SessionReport original{}, observed{};
    Ext4OrphanReclaimColdResult semantic{};
    block::SessionCounter reads{};
    usize verified_blocks{}, table_blocks{}, directory_blocks{};
    bool valid{};
};

// Serialized read-only capture through the owner's EXISTING Session. This does
// not acquire, close or replace that session, mutate its cache, or grant write
// authority. The owner excludes raw aliases, storage mutation and other session
// users while this object is alive. Any uncertainty quarantines that session.
// Writable cleanup must additionally reconcile cache identities and bind live
// publication/lifetime seals; a public report is never such an authority.
template <usize DeviceCapacity = 4>
class Ext4OrphanReclaimCapture final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
    template <usize> friend class Ext4ReclaimRecoveryBinding;
    template <usize> friend class Ext4ReclaimResultHistory;
    template <usize> friend class Ext4ReclaimComposedHistory;
    using Session = typename block::BlockSessions<DeviceCapacity>::Session;
    using Block = Ext4OrphanBlock;
    class UnguardedRead final
    {
      public:
        Status begin_read(Session &, u64, usize) { return {}; }
        Status finish_read(Session &, Status status) { return status; }
    };
  public:
    Ext4OrphanReclaimCapture() = default;
    Ext4OrphanReclaimCapture(const Ext4OrphanReclaimCapture &) = delete;
    Ext4OrphanReclaimCapture &operator=(const Ext4OrphanReclaimCapture &) = delete;
    Ext4OrphanReclaimCapture(Ext4OrphanReclaimCapture &&) = delete;
    Ext4OrphanReclaimCapture &operator=(Ext4OrphanReclaimCapture &&) = delete;

    [[nodiscard]] Status capture(Session &session, const Ext4RecoveryInfo &binding,
        Ext4OrphanReclaimCaptureStorage &storage, Ext4OrphanReclaimColdScratch &scratch)
    {
        UnguardedRead guard;
        return capture(session, binding, storage, scratch, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status capture(Session &session, const Ext4RecoveryInfo &binding,
        Ext4OrphanReclaimCaptureStorage &storage, Ext4OrphanReclaimColdScratch &scratch, Guard &guard)
    {
        if (attempted_) return Status::already_exists("reclaim capture is one-shot");
        attempted_ = true;
        if (auto status = session_anchor_.bind(session); !status) return status;
        original_outer_guard_ = outer_guard_; original_outer_context_ = outer_context_;
        report_.phase = Ext4OrphanReclaimCapturePhase::reading;
        session_ = &session; storage_ = &storage; scratch_ = &scratch; binding_ = original_binding_ = binding;
        const auto initial = session.report();
        if (!initial) return reject(initial.status());
        report_.original = report_.observed = initial.value(); original_io_ = initial.value();
        original_session_ = &session;
        const auto limit = session.transfer_limit();
        if (!limit) return reject(limit.status());
        if (binding.layout.first_sector || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks != 32768 || binding.metadata_count != 16 ||
            !binding.structural_metadata().valid() || !binding.needs_recovery || binding.empty_log ||
            binding.layout.journal_first_block < 3 || binding.layout.journal_first_block >= 32768 ||
            binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_blocks > 32768 - binding.layout.journal_first_block ||
            session.geometry().block_size != 512 || session.geometry().block_count < 65536 ||
            limit.value() < 1024)
            return reject(Status::unsupported("reclaim capture partition-relative profile"));
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = binding.metadata.data() + group * 4;
            if (metadata[1].count != 1 || metadata[2].count != 1 || metadata[3].count != 512)
                return reject(Status::unsupported("reclaim capture metadata placement"));
        }
        if (auto status = stable(); !status) return reject(status);
        if (auto status = read(1, storage.superblock, guard); !status) return reject(status);
        if (auto status = read(2, storage.descriptors, guard); !status) return reject(status);
        const u32 journal = u32(binding.layout.journal_first_block);
        if (auto status = read(journal, storage.head, guard); !status) return reject(status);
        const auto codec = jbd2::Codec::open(storage.head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return reject(codec.status());
        if (codec.value().superblock().start_block != 1 ||
            codec.value().superblock().sequence != binding.journal_sequence)
            return reject(Status::io_error("reclaim capture active journal identity"));
        if (auto status = read(journal + 1, storage.descriptor, guard); !status) return reject(status);
        if (auto status = codec.value().descriptor(storage.descriptor, binding.journal_sequence,
            [&](const jbd2::Tag &) -> Status {
                if (count_ == storage.payloads.size())
                    return Status::no_memory("reclaim capture journal participant budget");
                ++count_; return {};
            }); !status) return reject(status);
        if (count_ < 4 || count_ > 5)
            return reject(Status::unsupported("reclaim capture journal participant count"));
        for (usize index = 0; index < count_; ++index)
            if (auto status = read(journal + 2 + u32(index), storage.payloads[index], guard); !status) return reject(status);
        if (auto status = read(journal + 2 + u32(count_), storage.commit, guard); !status) return reject(status);
        if (auto status = read(journal + 3 + u32(count_), storage.end, guard); !status) return reject(status);
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = binding.metadata.data() + group * 4;
            const usize at = group * 32;
            if (ext4_checked::le32(storage.descriptors, at) != metadata[1].first ||
                ext4_checked::le32(storage.descriptors, at + 4) != metadata[2].first ||
                ext4_checked::le32(storage.descriptors, at + 8) != metadata[3].first)
                return reject(Status::io_error("reclaim capture descriptor binding differs"));
            if (auto status = read(u32(metadata[1].first), storage.block_bitmaps[group], guard); !status) return reject(status);
            if (auto status = read(u32(metadata[2].first), storage.inode_bitmaps[group], guard); !status) return reject(status);
            initialized_[group] = !(ext4_checked::le16(storage.descriptors, at + 18) & 1);
            if (initialized_[group])
                for (usize index = 0; index < 512; ++index)
                {
                    if (auto status = read(u32(metadata[3].first + index), storage.tables[group][index], guard); !status)
                        return reject(status);
                    ++report_.table_blocks;
                }
        }
        if (auto status = directories(guard); !status) return reject(status);
        Ext4OrphanReclaimColdResult semantic{};
        if (auto status = prove(semantic); !status) return reject(status);
        report_.semantic = semantic;
        establish_history();
        if (auto status = compare_all(guard); !status) return reject(status);
        report_.valid = true; report_.phase = Ext4OrphanReclaimCapturePhase::admitted;
        retained_.report = report_; retained_.terminal = true; admitted_ = true;
        if (auto status = retained_exact(); !status) return reject(status);
        return {};
    }

    // Re-read every retained whole source and rerun the full semantic proof.
    // There is no public escape hatch to replace the session or captured bytes.
    [[nodiscard]] Status revalidate()
    {
        UnguardedRead guard;
        return revalidate(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status revalidate(Guard &guard)
    {
        if (auto status = retained_exact(); !status) return reject(status);
        if (report_.phase != Ext4OrphanReclaimCapturePhase::admitted || !report_.valid)
            return Status::not_initialized("reclaim capture is not admitted");
        report_.phase = retained_.report.phase = Ext4OrphanReclaimCapturePhase::checking;
        if (auto status = compare_all(guard); !status) return reject(status);
        Ext4OrphanReclaimColdResult semantic{};
        if (auto status = prove(semantic); !status) return reject(status);
        if (auto status = stable(); !status) return reject(status);
        if (!reclaim_history::same_result(semantic, retained_.report.semantic))
            return reject(Status::io_error("reclaim retained semantic result changed"));
        report_.phase = retained_.report.phase = Ext4OrphanReclaimCapturePhase::admitted;
        if (auto status = retained_exact(); !status) return reject(status);
        return {};
    }
    [[nodiscard]] const Ext4OrphanReclaimCaptureReport &report() const { return report_; }

  private:
    [[nodiscard]] Status reject(Status status)
    {
        rejected_ = true;
        report_.valid = false; report_.semantic = {}; report_.status = status;
        report_.phase = Ext4OrphanReclaimCapturePhase::rejected;
        static_cast<void>(session_anchor_.abort(status));
        return status;
    }
    [[nodiscard]] Status stable()
    {
        if (!attempted_) return Status::fault("reclaim attempted latch rolled back");
        if (auto status = retained_exact(); !status) return status;
        if (outer_guard_) if (auto status = outer_guard_(outer_context_); !status) return status;
        if (session_ != original_session_ || !reclaim_history::same_session(report_.original, original_io_) ||
            !reclaim_history::same_info(binding_, original_binding_))
            return Status::fault("reclaim original Session observation changed");
        const auto current = session_->report();
        if (!current) return current.status();
        const auto &value = current.value(); const auto &old = report_.original;
        if (value.state != block::DeviceState::owned || value.first_error != StatusCode::ok ||
            value.in_flight || value.pending_writes || value.identity != old.identity ||
            value.registration_generation != old.registration_generation || value.session_serial != old.session_serial ||
            value.writes != old.writes || value.flushes != old.flushes ||
            value.completed_writes != old.completed_writes || value.completed_flushes != old.completed_flushes ||
            value.writes != value.completed_writes || value.flushes != value.completed_flushes ||
            old.reads > ~block::SessionCounter{0} - report_.reads ||
            value.reads != old.reads + report_.reads || value.completed_reads != value.reads)
            return Status::io_error("reclaim capture session counters or identity changed");
        report_.observed = value;
        if (established_) retained_.report.observed = value;
        return {};
    }
    [[nodiscard]] Status read(u32 physical, Block &bytes)
    {
        UnguardedRead guard;
        return read(physical, bytes, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status read(u32 physical, Block &bytes, Guard &guard)
    {
        if (physical >= 32768 || report_.reads == ~block::SessionCounter{0} ||
            report_.original.reads > ~block::SessionCounter{0} - report_.reads - 1)
            return Status::overflow("reclaim capture read bounds or counter");
        if (auto status = stable(); !status) return status;
        const auto previous = report_;
        const auto expected_guard = outer_guard_; const auto *expected_context = outer_context_;
        auto *source = storage_; auto *scratch = scratch_;
        const auto count = count_, directories = directory_count_;
        const auto initialized = initialized_;
        auto *original = original_session_;
        const auto io = block::guarded_read_blocks(*original, guard, u64{physical} * 2,
            std::as_writable_bytes(std::span{bytes}));
        if (!io) return io;
        if (!same_report(report_, previous) || session_ != original_session_ || storage_ != source ||
            scratch_ != scratch || outer_guard_ != expected_guard || outer_context_ != expected_context ||
            outer_guard_ != original_outer_guard_ || outer_context_ != original_outer_context_ ||
            !reclaim_history::same_info(binding_, original_binding_) || count_ != count || directory_count_ != directories || initialized_ != initialized)
            return Status::fault("reclaim read callback changed declared progress or identity");
        if (auto status = retained_exact(); !status) return status;
        if (outer_guard_) if (auto status = outer_guard_(outer_context_); !status) return status;
        ++report_.reads;
        if (established_) ++retained_.report.reads;
        return stable();
    }
    [[nodiscard]] Status directories()
    {
        UnguardedRead guard;
        return directories(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status directories(Guard &guard)
    {
        for (u32 number = 1; number <= 8192; ++number)
        {
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (!initialized_[group] || !(storage_->inode_bitmaps[group][local / 8] & (1U << (local % 8)))) continue;
            const auto inode = std::span<const u8>{storage_->tables[group][local / 4]}.subspan((local % 4) * 256, 256);
            if ((ext4_checked::le16(inode, 0) & 0170000) != 0040000) continue;
            const u32 size = ext4_checked::le32(inode, 4);
            const u16 entries = ext4_checked::le16(inode, 42);
            if (!size || size > 65536 || size % 1024 || ext4_checked::le32(inode, 108) ||
                ext4_checked::le16(inode, 40) != 0xf30a || entries > 4 || ext4_checked::le16(inode, 46))
                return Status::unsupported("reclaim capture directory mapping profile");
            for (u32 logical = 0; logical < size / 1024; ++logical)
            {
                if (directory_count_ == storage_->directories.size())
                    return Status::no_memory("reclaim capture directory budget");
                u32 physical{};
                for (u32 record = 0; record < entries; ++record)
                {
                    const usize at = 52 + record * 12;
                    const u32 first = ext4_checked::le32(inode, at), length = ext4_checked::le16(inode, at + 4);
                    const u32 start = ext4_checked::le32(inode, at + 8);
                    if (!length || length >= 0x8000 || ext4_checked::le16(inode, at + 6) ||
                        start >= 32768 || length > 32768 - start)
                        return Status::unsupported("reclaim capture directory extent bounds");
                    if (logical >= first && logical - first < length)
                    {
                        if (physical) return Status::io_error("reclaim capture duplicate directory mapping");
                        physical = start + logical - first;
                    }
                }
                if (!physical) return Status::io_error("reclaim capture directory hole");
                auto &entry = storage_->directories[directory_count_++];
                entry.inode = number; entry.logical = logical; entry.physical = physical;
                if (auto status = read(physical, entry.bytes, guard); !status) return status;
            }
        }
        report_.directory_blocks = directory_count_; return {};
    }
    [[nodiscard]] Status prove(Ext4OrphanReclaimColdResult &semantic)
    {
        std::array<std::span<const Block>, 4> tables{};
        for (usize group = 0; group < 4; ++group) if (initialized_[group]) tables[group] = storage_->tables[group];
        return Ext4OrphanReclaimColdVerifier::verify(binding_,
            {storage_->head, storage_->descriptor, storage_->commit, storage_->end,
                std::span<const Block>{storage_->payloads}.first(count_)},
            {storage_->superblock, storage_->descriptors, storage_->inode_bitmaps, storage_->block_bitmaps, tables,
                std::span<const Ext4OrphanDirectoryBlockImage>{storage_->directories}.first(directory_count_)},
            *scratch_, semantic);
    }
    [[nodiscard]] Status compare(u32 physical, const Block &expected)
    {
        UnguardedRead guard;
        return compare(physical, expected, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare(u32 physical, const Block &expected, Guard &guard)
    {
        if (auto status = read(physical, checking_, guard); !status) return status;
        if (checking_ != expected) return Status::io_error("reclaim capture whole device block changed");
        if (report_.verified_blocks == ~usize{0}) return Status::overflow("reclaim verified block count");
        ++report_.verified_blocks; ++retained_.report.verified_blocks; return {};
    }
    [[nodiscard]] Status compare_all()
    {
        UnguardedRead guard;
        return compare_all(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_all(Guard &guard)
    {
        if (auto status = compare(1, storage_->superblock, guard); !status) return status;
        if (auto status = compare(2, storage_->descriptors, guard); !status) return status;
        const u32 journal = u32(binding_.layout.journal_first_block);
        if (auto status = compare(journal, storage_->head, guard); !status) return status;
        if (auto status = compare(journal + 1, storage_->descriptor, guard); !status) return status;
        for (usize index = 0; index < count_; ++index)
            if (auto status = compare(journal + 2 + u32(index), storage_->payloads[index], guard); !status) return status;
        if (auto status = compare(journal + 2 + u32(count_), storage_->commit, guard); !status) return status;
        if (auto status = compare(journal + 3 + u32(count_), storage_->end, guard); !status) return status;
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = binding_.metadata.data() + group * 4;
            if (auto status = compare(u32(metadata[1].first), storage_->block_bitmaps[group], guard); !status) return status;
            if (auto status = compare(u32(metadata[2].first), storage_->inode_bitmaps[group], guard); !status) return status;
            if (initialized_[group])
                for (usize index = 0; index < 512; ++index)
                    if (auto status = compare(u32(metadata[3].first + index), storage_->tables[group][index], guard); !status) return status;
        }
        for (usize index = 0; index < directory_count_; ++index)
            if (auto status = compare(storage_->directories[index].physical, storage_->directories[index].bytes, guard); !status) return status;
        return stable();
    }

    struct Retained
    {
        Retained() = default;
        Retained(const Retained &) = delete;
        Retained &operator=(const Retained &) = delete;
        Retained(Retained &&) = delete;
        Retained &operator=(Retained &&) = delete;
        Ext4OrphanReclaimCaptureStorage storage{};
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        Ext4OrphanReclaimJournalPlan journal{};
        std::array<Block, 5> before{}, after{}, current{};
        Block normalized{}, old_head{};
        Ext4RecoveryInfo binding{};
        Ext4OrphanReclaimCaptureReport report{};
        Session *session{};
        Ext4OrphanReclaimCaptureStorage *source{};
        Ext4OrphanReclaimColdScratch *scratch{};
        std::array<bool, 4> initialized{};
        usize count{}, directories{};
        Status (*outer_guard)(const void *){};
        const void *outer_context{};
        bool ready{}, terminal{};
    };
    static bool same_report(const Ext4OrphanReclaimCaptureReport &a, const Ext4OrphanReclaimCaptureReport &b)
    {
        return a.phase == b.phase && reclaim_history::same_status(a.status, b.status) &&
            reclaim_history::same_session(a.original, b.original) && reclaim_history::same_session(a.observed, b.observed) &&
            reclaim_history::same_result(a.semantic, b.semantic) && a.reads == b.reads &&
            a.verified_blocks == b.verified_blocks && a.table_blocks == b.table_blocks &&
            a.directory_blocks == b.directory_blocks && a.valid == b.valid;
    }
    static bool same_storage(const Ext4OrphanReclaimCaptureStorage &a, const Ext4OrphanReclaimCaptureStorage &b)
    {
        if (a.superblock != b.superblock || a.descriptors != b.descriptors || a.head != b.head ||
            a.descriptor != b.descriptor || a.commit != b.commit || a.end != b.end || a.payloads != b.payloads ||
            a.inode_bitmaps != b.inode_bitmaps || a.block_bitmaps != b.block_bitmaps || a.tables != b.tables)
            return false;
        for (usize index = 0; index < a.directories.size(); ++index)
            if (a.directories[index].inode != b.directories[index].inode ||
                a.directories[index].logical != b.directories[index].logical ||
                a.directories[index].physical != b.directories[index].physical ||
                a.directories[index].bytes != b.directories[index].bytes) return false;
        return true;
    }
    void establish_history()
    {
        retained_.storage = *storage_; retained_.binding = binding_; retained_.report = report_;
        retained_.plan = scratch_->reclaim; retained_.journal = scratch_->canonical;
        retained_.before = scratch_->before; retained_.after = scratch_->after; retained_.current = scratch_->current;
        retained_.normalized = scratch_->normalized; retained_.old_head = scratch_->old_head;
        retained_.session = session_; retained_.source = storage_; retained_.scratch = scratch_;
        retained_.initialized = initialized_; retained_.count = count_; retained_.directories = directory_count_;
        retained_.outer_guard = outer_guard_; retained_.outer_context = outer_context_;
        retained_.ready = true; established_ = true;
    }
    // Pure exactness survives later Owner writes; Session counters belong to
    // stable(), which is used only during this serialized read-only borrow.
    [[nodiscard]] Status retained_exact() const
    {
        if (rejected_) return Status::io_error("reclaim retained capture is terminally rejected");
        if (!session_anchor_.exact(session_) || !session_anchor_.exact(original_session_))
            return Status::fault("reclaim capture original Session borrow anchors changed");
        if (!established_) return retained_.ready ? Status::fault("reclaim capture seal latch differs") : Status{};
        if (!attempted_ || !retained_.ready || retained_.terminal != admitted_ || session_ != retained_.session || storage_ != retained_.source ||
            scratch_ != retained_.scratch || original_session_ != retained_.session ||
            !reclaim_history::same_session(original_io_, retained_.report.original) || outer_guard_ != retained_.outer_guard ||
            outer_context_ != retained_.outer_context || original_outer_guard_ != retained_.outer_guard ||
            original_outer_context_ != retained_.outer_context || !storage_ || !scratch_ || count_ != retained_.count ||
            count_ > storage_->payloads.size() || directory_count_ != retained_.directories ||
            directory_count_ > storage_->directories.size() || initialized_ != retained_.initialized ||
            !reclaim_history::same_info(binding_, retained_.binding) ||
            !reclaim_history::same_info(original_binding_, retained_.binding) || !same_report(report_, retained_.report) ||
            (retained_.terminal && report_.phase != Ext4OrphanReclaimCapturePhase::admitted &&
             report_.phase != Ext4OrphanReclaimCapturePhase::checking) ||
            !same_storage(*storage_, retained_.storage) ||
            !reclaim_history::same_plan(scratch_->reclaim.reclaim, retained_.plan.reclaim) ||
            !reclaim_history::same_directory(scratch_->reclaim.directory, retained_.plan.directory) ||
            scratch_->reclaim.valid != retained_.plan.valid ||
            !reclaim_history::same_journal(scratch_->canonical, retained_.journal) ||
            scratch_->before != retained_.before || scratch_->after != retained_.after ||
            scratch_->current != retained_.current || scratch_->normalized != retained_.normalized ||
            scratch_->old_head != retained_.old_head)
            return Status::fault("reclaim whole retained capture or interpretation changed");
        return {};
    }

    reclaim_history::SessionAnchor<Session> session_anchor_{};
    Session *session_{}, *original_session_{};
    block::SessionReport original_io_{};
    Ext4OrphanReclaimCaptureStorage *storage_{};
    Ext4OrphanReclaimColdScratch *scratch_{};
    Ext4RecoveryInfo binding_{}, original_binding_{};
    Ext4OrphanReclaimCaptureReport report_{};
    std::array<bool, 4> initialized_{};
    usize count_{}, directory_count_{};
    Block checking_{};
    // This complete object is caller-owned off stack, like capture storage.
    Retained retained_{};
    bool attempted_{}, established_{}, rejected_{}, admitted_{};
    Status (*outer_guard_)(const void *){};
    const void *outer_context_{};
    Status (*original_outer_guard_)(const void *){};
    const void *original_outer_context_{};
};

// Exclusive kernel-owned storage; neither this nor semantic scratch is on stack.
struct Ext4ReclaimRecoveryStorage
{
    Ext4ReclaimRecoveryStorage() = default;
    Ext4ReclaimRecoveryStorage(const Ext4ReclaimRecoveryStorage &) = delete;
    Ext4ReclaimRecoveryStorage &operator=(const Ext4ReclaimRecoveryStorage &) = delete;
    Ext4ReclaimRecoveryStorage(Ext4ReclaimRecoveryStorage &&) = delete;
    Ext4ReclaimRecoveryStorage &operator=(Ext4ReclaimRecoveryStorage &&) = delete;
    struct Observation { u32 physical{}; Ext4OrphanBlock bytes{}; };
    std::array<Observation, 12> bootstrap_reads{};
    Ext4OrphanBlock superblock{}, descriptors{}, journal_inode{}, head{}, descriptor{}, commit{}, end{};
    Ext4OrphanBlock old_head{}, normalized{}, readback{}, parser_record{}, parser_payload{};
    std::array<Ext4OrphanBlock, 5> payloads{}, before{}, after{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4OrphanReclaimCaptureStorage actual{};
};

enum class Ext4ReclaimRecoveryPhase : u8 { unused, bootstrap, journal, before_binding, capturing, ready, rejected };
struct Ext4ReclaimRecoveryReport
{
    Ext4ReclaimRecoveryPhase phase{Ext4ReclaimRecoveryPhase::unused};
    Status status{};
    block::SessionReport original{}, observed{};
    block::SessionCounter reads{};
    usize bootstrap_observations{};
    bool recognized_marker{}, valid{};
};

// Read-only actual-media admission. Reports never grant writable authority.
// The caller excludes raw device/Session/storage aliases throughout this borrow.
template <usize DeviceCapacity = 4>
class Ext4ReclaimRecoveryBinding final
{
    template <usize> friend class Ext4ReclaimResultHistory;
    template <usize> friend class Ext4ReclaimComposedHistory;
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4OrphanBlock;
    using Session = typename block::BlockSessions<DeviceCapacity>::Session;
    using UnguardedRead = typename Ext4OrphanReclaimCapture<DeviceCapacity>::UnguardedRead;
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    class BeforeDevice final : public driver::BlockDevice
    {
      public:
        explicit BeforeDevice(Ext4ReclaimRecoveryBinding &owner, Guard &guard) : owner_(owner), guard_(guard) {}
        driver::BlockGeometry geometry() const override { return owner_.session_->geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            if (first % 2 || out.size() != 1024 || first / 2 >= 32768)
                return Status::invalid_argument("reclaim bootstrap read bounds");
            auto &storage = *owner_.storage_;
            if (owner_.report_.bootstrap_observations == storage.bootstrap_reads.size())
                return Status::no_memory("reclaim bootstrap observation capacity");
            auto &observation = storage.bootstrap_reads[owner_.report_.bootstrap_observations];
            observation.physical = u32(first / 2);
            if (auto status = owner_.read(observation.physical, observation.bytes, guard_); !status) return status;
            ++owner_.report_.bootstrap_observations;
            const Block *image = &observation.bytes;
            if (observation.physical == 1) image = &storage.normalized;
            else if (observation.physical == owner_.journal_first_) image = &storage.old_head;
            else for (usize index = 0; index < owner_.count_; ++index)
                if (owner_.targets_[index] == observation.physical) image = &storage.before[index];
            for (usize at = 0; at < 1024; ++at) out[at] = std::byte((*image)[at]);
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("reclaim bootstrap overlay is readonly"); }
        Status flush() override { return Status::denied("reclaim bootstrap overlay is readonly"); }
      private:
        Ext4ReclaimRecoveryBinding &owner_;
        Guard &guard_;
    };
    struct JournalReader
    {
        Ext4ReclaimRecoveryBinding &owner;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            const auto &storage = *owner.storage_;
            const Block *image{};
            if (bytes.size() != 1024) return Status::invalid_argument("reclaim journal record size");
            if (!number) image = &storage.head;
            else if (number == 1) image = &storage.descriptor;
            else if (number >= 2 && number < owner.count_ + 2) image = &storage.payloads[number - 2];
            else if (number == owner.count_ + 2) image = &storage.commit;
            else if (number == owner.count_ + 3) image = &storage.end;
            if (!image) return Status::io_error("reclaim journal record absent");
            for (usize at = 0; at < 1024; ++at) bytes[at] = (*image)[at];
            return {};
        }
    };
  public:
    Ext4ReclaimRecoveryBinding() = default;
    Ext4ReclaimRecoveryBinding(const Ext4ReclaimRecoveryBinding &) = delete;
    Ext4ReclaimRecoveryBinding &operator=(const Ext4ReclaimRecoveryBinding &) = delete;
    Ext4ReclaimRecoveryBinding(Ext4ReclaimRecoveryBinding &&) = delete;
    Ext4ReclaimRecoveryBinding &operator=(Ext4ReclaimRecoveryBinding &&) = delete;

    [[nodiscard]] Status inspect(Session &session, Ext4ReclaimRecoveryStorage &storage,
                                 Ext4OrphanReclaimColdScratch &scratch)
    {
        UnguardedRead guard;
        return inspect(session, storage, scratch, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status inspect(Session &session, Ext4ReclaimRecoveryStorage &storage,
                                 Ext4OrphanReclaimColdScratch &scratch, Guard &guard)
    {
        if (attempted_) return Status::already_exists("reclaim recovery binding is one-shot");
        attempted_ = true;
        if (auto status = session_anchor_.bind(session); !status) return status;
        session_ = &session; storage_ = &storage; scratch_ = &scratch;
        report_.phase = Ext4ReclaimRecoveryPhase::bootstrap;
        const auto initial = session.report();
        if (!initial) return reject(initial.status());
        report_.original = report_.observed = initial.value(); original_io_ = initial.value();
        original_session_ = &session;
        if (auto status = stable(); !status) return reject(status);
        const auto limit = session.transfer_limit();
        if (!limit) return reject(limit.status());
        if (session.geometry().block_size != 512 || session.geometry().block_count < 65536 || limit.value() < 1024)
            return reject(Status::unsupported("reclaim recovery partition geometry"));
        if (auto status = bootstrap(guard); !status) return reject(status);
        if (auto status = records(guard); !status) return reject(status);
        report_.phase = Ext4ReclaimRecoveryPhase::before_binding;
        storage.normalized = storage.before[count_ - 1];
        store32(storage.normalized, 232, 0);
        store32(storage.normalized, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{storage.normalized}.first(1020)));
        storage.old_head = storage.head;
        big32(storage.old_head, 24, sequence_ - 1); big32(storage.old_head, 28, 0);
        big32(storage.old_head, 252, 0);
        big32(storage.old_head, 252, jbd2::detail::crc32c(~u32{0}, storage.old_head));
        retain_records();
        BeforeDevice<Guard> overlay(*this, guard);
        if (auto status = bootstrap_.inspect(overlay, {0, session.geometry().block_count}); !status) return reject(status);
        const auto *old = bootstrap_.info();
        if (!old || !old->needs_recovery || !old->empty_log || old->uuid != uuid_ ||
            old->journal_sequence != sequence_ - 1 || old->journal_inode != 8 || old->metadata_count != 16 ||
            old->layout.first_sector || old->layout.block_size != 1024 || old->layout.filesystem_blocks != 32768 ||
            old->layout.journal_first_block != journal_first_ || old->layout.journal_blocks != journal_blocks_)
            return reject(Status::io_error("reclaim recovery strict before binding differs"));
        binding_ = *old; binding_.empty_log = false; binding_.journal_sequence = sequence_;
        if (auto status = stable(); !status) return reject(status);
        report_.phase = Ext4ReclaimRecoveryPhase::capturing;
        establish_history();
        capture_.outer_guard_ = &capture_outer_guard;
        capture_.outer_context_ = this;
        if (auto status = capture_.capture(session, binding_, storage.actual, scratch, guard); !status) return reject(status);
        if (auto status = retained_exact(false); !status) return reject(status);
        if (capture_.outer_guard_ != &capture_outer_guard || capture_.outer_context_ != this ||
            !reclaim_history::same_info(capture_.binding_, binding_))
            return reject(Status::fault("reclaim nested original binding lineage changed"));
        retained_.capture_report = capture_.report(); capture_admitted_ = retained_.capture_admitted = true;
        if (auto status = adopt_capture_reads(capture_.report().reads); !status) return reject(status);
        if (auto status = initial_matches(); !status) return reject(status);
        if (auto status = compare_bootstrap(guard); !status) return reject(status);
        report_.phase = Ext4ReclaimRecoveryPhase::ready; report_.valid = true;
        retained_.report = report_; retained_.terminal = true; admitted_ = true;
        if (auto status = retained_exact(); !status) return reject(status);
        return {};
    }
    [[nodiscard]] Status revalidate()
    {
        UnguardedRead guard;
        return revalidate(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status revalidate(Guard &guard)
    {
        if (auto status = retained_exact(); !status) return reject(status);
        if (!established_ || rejected_)
            return Status::not_initialized("reclaim recovery binding is not admitted");
        if (!report_.valid || report_.phase != Ext4ReclaimRecoveryPhase::ready)
            return Status::not_initialized("reclaim recovery binding is not admitted");
        if (auto status = compare_bootstrap(guard); !status) return reject(status);
        if (auto status = compare_captured(guard); !status) return reject(status);
        if (auto status = prove_retained(); !status) return reject(status);
        if (auto status = initial_matches(); !status) return reject(status);
        return {};
    }
    [[nodiscard]] const Ext4ReclaimRecoveryReport &report() const { return report_; }
    [[nodiscard]] const Ext4RecoveryInfo *info() const { return report_.valid && retained_.terminal && retained_exact() ? &binding_ : nullptr; }
    [[nodiscard]] const Ext4OrphanReclaimColdResult *proof() const
    { return report_.valid && retained_.terminal && retained_exact() ? &capture_.report().semantic : nullptr; }

  private:
    static void store32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] Status reject(Status status)
    {
        rejected_ = true;
        report_.valid = false; report_.status = status; report_.phase = Ext4ReclaimRecoveryPhase::rejected;
        static_cast<void>(session_anchor_.abort(status));
        return status;
    }
    [[nodiscard]] Status stable()
    {
        if (!attempted_) return Status::fault("reclaim attempted latch rolled back");
        if (auto status = retained_exact(); !status) return status;
        if (session_ != original_session_ || !reclaim_history::same_session(report_.original, original_io_))
            return Status::fault("reclaim binding original Session observation changed");
        const auto current = session_->report();
        if (!current) return current.status();
        const auto &now = current.value(); const auto &old = report_.original;
        if (now.state != block::DeviceState::owned || now.first_error != StatusCode::ok || now.in_flight ||
            now.pending_writes || now.identity != old.identity || now.registration_generation != old.registration_generation ||
            now.session_serial != old.session_serial || now.writes != old.writes || now.flushes != old.flushes ||
            now.completed_writes != old.completed_writes || now.completed_flushes != old.completed_flushes ||
            now.writes != now.completed_writes || now.flushes != now.completed_flushes ||
            old.reads > ~block::SessionCounter{0} - report_.reads || now.reads != old.reads + report_.reads || now.completed_reads != now.reads)
            return Status::io_error("reclaim recovery Session identity or exact counters differ");
        report_.observed = now;
        if (established_) retained_.report.observed = now;
        return {};
    }
    [[nodiscard]] Status read(u32 physical, Block &image)
    {
        UnguardedRead guard;
        return read(physical, image, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status read(u32 physical, Block &image, Guard &guard)
    {
        if (physical >= 32768 || report_.reads == ~block::SessionCounter{0} ||
            report_.original.reads > ~block::SessionCounter{0} - report_.reads - 1)
            return Status::overflow("reclaim recovery read bounds or counter");
        if (auto status = stable(); !status) return status;
        const auto previous = report_;
        auto *source = storage_; auto *scratch = scratch_;
        auto *original = original_session_;
        const auto io = block::guarded_read_blocks(*original, guard, u64{physical} * 2,
            std::as_writable_bytes(std::span{image}));
        if (!io) return io;
        if (!same_report(report_, previous) || session_ != original_session_ || storage_ != source || scratch_ != scratch)
            return Status::fault("reclaim binding read callback changed declared progress or identity");
        if (auto status = retained_exact(); !status) return status;
        ++report_.reads;
        if (established_) ++retained_.report.reads;
        return stable();
    }
    [[nodiscard]] Status adopt_capture_reads(block::SessionCounter count)
    {
        if (count > ~block::SessionCounter{0} - report_.reads) return Status::overflow("reclaim recovery capture read count");
        if (auto status = retained_exact(); !status) return status;
        report_.reads += count;
        retained_.report.reads += count;
        return stable();
    }
    [[nodiscard]] Status bootstrap()
    {
        UnguardedRead guard;
        return bootstrap(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status bootstrap(Guard &guard)
    {
        auto &s = *storage_;
        if (auto status = read(1, s.superblock, guard); !status) return status;
        if (ext4_checked::le16(s.superblock, 56) != 0xef53 ||
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{s.superblock}.first(1020)) != ext4_checked::le32(s.superblock, 1020) ||
            ext4_checked::le32(s.superblock, 24) || ext4_checked::le32(s.superblock, 4) != 32768 ||
            ext4_checked::le32(s.superblock, 0) != 8192 || ext4_checked::le32(s.superblock, 20) != 1 ||
            ext4_checked::le32(s.superblock, 32) != 8192 || ext4_checked::le32(s.superblock, 40) != 2048 ||
            ext4_checked::le16(s.superblock, 88) != 256 || ext4_checked::le32(s.superblock, 92) != 4 ||
            ext4_checked::le32(s.superblock, 96) != 0x46 || ext4_checked::le32(s.superblock, 100) != 0x400 ||
            ext4_checked::le32(s.superblock, 224) != 8 || s.superblock[373] != 1)
            return Status::unsupported("reclaim recovery checksummed bootstrap superblock");
        for (usize index = 0; index < uuid_.size(); ++index) uuid_[index] = s.superblock[104 + index];
        if (auto status = read(2, s.descriptors, guard); !status) return status;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid_);
        Block descriptor = s.descriptors;
        const auto checksum = ext4_checked::le16(descriptor, 30); descriptor[30] = descriptor[31] = 0;
        if (u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
                std::span<const u8>{descriptor}.first(32))) != checksum)
            return Status::io_error("reclaim recovery group-zero descriptor checksum");
        const u32 table = ext4_checked::le32(s.descriptors, 8);
        if (table < 3 || table >= 32768 - 512) return Status::invalid_argument("reclaim recovery journal table bounds");
        journal_table_ = table + 1;
        if (auto status = read(journal_table_, s.journal_inode, guard); !status) return status;
        const auto inode = std::span<const u8>{s.journal_inode}.subspan(3 * 256, 256);
        if (auto status = ext4_checked::inode_checksum(seed, 8, inode); !status) return status;
        journal_blocks_ = ext4_checked::le16(inode, 56); journal_first_ = ext4_checked::le32(inode, 60);
        if (ext4_checked::le16(inode, 0) != 0100600 || ext4_checked::le16(inode, 26) != 1 ||
            ext4_checked::le32(inode, 20) || ext4_checked::le32(inode, 32) != 0x80000 ||
            ext4_checked::le16(inode, 40) != 0xf30a || ext4_checked::le16(inode, 42) != 1 ||
            ext4_checked::le16(inode, 44) != 4 || ext4_checked::le16(inode, 46) ||
            ext4_checked::le32(inode, 52) || ext4_checked::le16(inode, 58) ||
            journal_blocks_ < 1024 || journal_blocks_ > 32768 || journal_first_ < 3 || journal_first_ >= 32768 ||
            journal_blocks_ > 32768 - journal_first_ || ext4_checked::le32(inode, 4) != journal_blocks_ * 1024 ||
            ext4_checked::le32(inode, 28) != journal_blocks_ * 2)
            return Status::unsupported("reclaim recovery contiguous journal inode");
        return {};
    }
    [[nodiscard]] Status records()
    {
        UnguardedRead guard;
        return records(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status records(Guard &guard)
    {
        auto &s = *storage_; report_.phase = Ext4ReclaimRecoveryPhase::journal;
        if (auto status = read(journal_first_, s.head, guard); !status) return status;
        const auto codec = jbd2::Codec::open(s.head, journal_blocks_, uuid_, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock(); sequence_ = head.sequence;
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            sequence_ < 2 || sequence_ > 0xfffffffdU)
            return Status::unsupported("reclaim recovery active journal sequence or ring");
        if (auto status = read(journal_first_ + 1, s.descriptor, guard); !status) return status;
        if (!Ext4OrphanReclaimWitnessCodec::present(s.descriptor))
            return Status::unsupported("reclaim recovery private marker absent");
        report_.recognized_marker = true;
        if (auto status = codec.value().descriptor(s.descriptor, sequence_, [&](const jbd2::Tag &tag) -> Status {
                if (count_ == targets_.size() || tag.target_block >= 32768 ||
                    (tag.target_block >= journal_first_ && tag.target_block - journal_first_ < journal_blocks_))
                    return Status::invalid_argument("reclaim recovery journal target bounds");
                for (usize prior = 0; prior < count_; ++prior)
                    if (targets_[prior] == tag.target_block) return Status::io_error("reclaim recovery duplicate target");
                targets_[count_++] = u32(tag.target_block); return {};
            }); !status) return status;
        if (count_ < 4 || count_ > 5 || targets_[count_ - 1] != 1)
            return Status::unsupported("reclaim recovery participant profile");
        for (usize index = 0; index < count_; ++index)
            if (auto status = read(journal_first_ + u32(index) + 2, s.payloads[index], guard); !status) return status;
        if (auto status = read(journal_first_ + u32(count_) + 2, s.commit, guard); !status) return status;
        if (auto status = read(journal_first_ + u32(count_) + 3, s.end, guard); !status) return status;
        const u64 end = u64{journal_first_} + journal_blocks_;
        const std::array<jbd2::BlockRange, 2> domain{{{0, journal_first_}, {end, 32768 - end}}};
        JournalReader reader{*this};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {32768, std::span<const jbd2::BlockRange>{domain}.first(end < 32768 ? 2 : 1)},
            {s.parser_record, s.parser_payload, s.actions, s.revocations});
        if (!parsed) return parsed.status();
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 || replay.revoked_actions || replay.revoke_records ||
            replay.actions.size() != count_ || replay.next_sequence != sequence_ + 1 || !jbd2::detail::all_zero(s.end))
            return Status::io_error("reclaim recovery journal grammar");
        for (usize index = 0; index < count_; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != targets_[index] || action.journal_block != index + 2 ||
                action.sequence != sequence_ || action.transaction_order)
                return Status::io_error("reclaim recovery journal action identity");
            s.after[index] = s.payloads[index];
            if (action.restore_magic) big32(s.after[index], 0, jbd2::magic);
        }
        Ext4OrphanReclaimWitness witness{};
        if (auto status = Ext4OrphanReclaimWitnessCodec::decode(s.descriptor, uuid_,
                std::span<const Block>{s.after}.first(count_), std::span<Block>{s.before}.first(count_), witness); !status)
            return status;
        if (witness.count != count_ || witness.sequence != sequence_)
            return Status::io_error("reclaim recovery witness sequence or count");
        for (usize index = 0; index < count_; ++index)
            if (witness.targets[index] != targets_[index]) return Status::io_error("reclaim recovery witness target differs");
        return {};
    }
    [[nodiscard]] Status initial_matches() const
    {
        const auto &s = *storage_;
        if (s.superblock != s.actual.superblock || s.descriptors != s.actual.descriptors ||
            s.head != s.actual.head || s.descriptor != s.actual.descriptor || s.commit != s.actual.commit || s.end != s.actual.end)
            return Status::io_error("reclaim recovery initial capture changed");
        for (usize index = 0; index < count_; ++index)
            if (s.payloads[index] != s.actual.payloads[index]) return Status::io_error("reclaim recovery payload changed");
        return {};
    }
    [[nodiscard]] Status equal(u32 physical, const Block &expected)
    {
        UnguardedRead guard;
        return equal(physical, expected, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status equal(u32 physical, const Block &expected, Guard &guard)
    {
        if (auto status = read(physical, storage_->readback, guard); !status) return status;
        return storage_->readback == expected ? Status{} : Status::io_error("reclaim recovery bootstrap observation changed");
    }
    [[nodiscard]] Status compare_bootstrap()
    {
        UnguardedRead guard;
        return compare_bootstrap(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_bootstrap(Guard &guard)
    {
        const auto &s = *storage_;
        if (auto status = equal(1, s.superblock, guard); !status) return status;
        if (auto status = equal(2, s.descriptors, guard); !status) return status;
        if (auto status = equal(journal_table_, s.journal_inode, guard); !status) return status;
        for (usize index = 0; index < report_.bootstrap_observations; ++index)
            if (auto status = equal(s.bootstrap_reads[index].physical, s.bootstrap_reads[index].bytes, guard); !status) return status;
        return stable();
    }
    [[nodiscard]] Status compare_captured()
    {
        UnguardedRead guard;
        return compare_captured(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_captured(Guard &guard)
    {
        const auto &s = storage_->actual;
        if (auto status = equal(1, s.superblock, guard); !status) return status;
        if (auto status = equal(2, s.descriptors, guard); !status) return status;
        if (auto status = equal(journal_first_, s.head, guard); !status) return status;
        if (auto status = equal(journal_first_ + 1, s.descriptor, guard); !status) return status;
        for (usize index = 0; index < count_; ++index)
            if (auto status = equal(journal_first_ + u32(index) + 2, s.payloads[index], guard); !status) return status;
        if (auto status = equal(journal_first_ + u32(count_) + 2, s.commit, guard); !status) return status;
        if (auto status = equal(journal_first_ + u32(count_) + 3, s.end, guard); !status) return status;
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = binding_.metadata.data() + group * 4;
            if (auto status = equal(u32(metadata[1].first), s.block_bitmaps[group], guard); !status) return status;
            if (auto status = equal(u32(metadata[2].first), s.inode_bitmaps[group], guard); !status) return status;
            if (!(ext4_checked::le16(s.descriptors, group * 32 + 18) & 1))
                for (usize index = 0; index < 512; ++index)
                    if (auto status = equal(u32(metadata[3].first + index), s.tables[group][index], guard); !status) return status;
        }
        for (usize index = 0; index < capture_.report().directory_blocks; ++index)
            if (auto status = equal(s.directories[index].physical, s.directories[index].bytes, guard); !status) return status;
        return stable();
    }
    [[nodiscard]] Status prove_retained()
    {
        const auto &s = storage_->actual;
        std::array<std::span<const Block>, 4> tables{};
        for (usize group = 0; group < 4; ++group)
            if (!(ext4_checked::le16(s.descriptors, group * 32 + 18) & 1)) tables[group] = s.tables[group];
        Ext4OrphanReclaimColdResult checked{};
        if (auto status = Ext4OrphanReclaimColdVerifier::verify(binding_,
                {s.head, s.descriptor, s.commit, s.end, std::span<const Block>{s.payloads}.first(count_)},
                {s.superblock, s.descriptors, s.inode_bitmaps, s.block_bitmaps, tables,
                 std::span<const Ext4OrphanDirectoryBlockImage>{s.directories}.first(capture_.report().directory_blocks)},
                *scratch_, checked); !status) return status;
        const auto &original = capture_.report().semantic;
        if (!checked.valid || checked.targets != original.targets || checked.home_after != original.home_after ||
            checked.inode != original.inode || checked.old_generation != original.old_generation ||
            checked.retained_generation != original.retained_generation || checked.next_head != original.next_head ||
            checked.active_sequence != original.active_sequence || checked.parsed_next_sequence != original.parsed_next_sequence ||
            checked.cleanup_sequence != original.cleanup_sequence || checked.count != original.count ||
            checked.old_homes != original.old_homes || checked.new_homes != original.new_homes ||
            checked.witness_runs != original.witness_runs || checked.witness_bytes != original.witness_bytes ||
            checked.all_before != original.all_before || checked.all_after != original.all_after ||
            checked.complete_global_before_proof != original.complete_global_before_proof)
            return Status::io_error("reclaim recovery retained semantic identity differs");
        return stable();
    }
    struct Retained
    {
        Retained() = default;
        Retained(const Retained &) = delete;
        Retained &operator=(const Retained &) = delete;
        Retained(Retained &&) = delete;
        Retained &operator=(Retained &&) = delete;
        std::array<Ext4ReclaimRecoveryStorage::Observation, 12> bootstrap{};
        Block super{}, descriptors{}, journal_inode{}, head{}, descriptor{}, commit{}, end{};
        Block old_head{}, normalized{}, parser_record{}, parser_payload{};
        std::array<Block, 5> payloads{}, before{}, after{};
        std::array<jbd2::ReplayAction, 5> actions{};
        std::array<jbd2::Revocation, 1> revocations{};
        Ext4RecoveryInfo binding{}, bootstrap_binding{};
        Ext4ReclaimRecoveryReport report{};
        Ext4OrphanReclaimCaptureReport capture_report{};
        Session *session{};
        Ext4ReclaimRecoveryStorage *storage{};
        Ext4OrphanReclaimColdScratch *scratch{};
        driver::BlockGeometry geometry{};
        std::array<u8, 16> uuid{};
        std::array<u32, 5> targets{};
        usize count{};
        u32 journal_first{}, journal_blocks{}, journal_table{}, sequence{};
        bool ready{}, terminal{}, capture_admitted{}, records_ready{};
    };
    static bool same_report(const Ext4ReclaimRecoveryReport &a, const Ext4ReclaimRecoveryReport &b)
    {
        return a.phase == b.phase && reclaim_history::same_status(a.status, b.status) &&
            reclaim_history::same_session(a.original, b.original) && reclaim_history::same_session(a.observed, b.observed) &&
            a.reads == b.reads && a.bootstrap_observations == b.bootstrap_observations &&
            a.recognized_marker == b.recognized_marker && a.valid == b.valid;
    }
    static Status capture_outer_guard(const void *context)
    { return static_cast<const Ext4ReclaimRecoveryBinding *>(context)->retained_exact(false); }
    void retain_records()
    {
        const auto &s = *storage_;
        retained_.super = s.superblock;
        retained_.descriptors = s.descriptors; retained_.journal_inode = s.journal_inode;
        retained_.head = s.head; retained_.descriptor = s.descriptor; retained_.commit = s.commit;
        retained_.end = s.end; retained_.old_head = s.old_head; retained_.normalized = s.normalized;
        retained_.parser_record = s.parser_record; retained_.parser_payload = s.parser_payload;
        retained_.payloads = s.payloads; retained_.before = s.before; retained_.after = s.after;
        retained_.actions = s.actions; retained_.revocations = s.revocations;

        retained_.storage = storage_; retained_.uuid = uuid_; retained_.targets = targets_;
        retained_.count = count_; retained_.journal_first = journal_first_;
        retained_.journal_blocks = journal_blocks_; retained_.journal_table = journal_table_;
        retained_.sequence = sequence_; retained_.records_ready = true; records_sealed_ = true;
    }
    void establish_history()
    {
        retained_.bootstrap = storage_->bootstrap_reads;
        retained_.binding = binding_; retained_.bootstrap_binding = *bootstrap_.info();
        retained_.report = report_; retained_.session = session_; retained_.storage = storage_;
        retained_.scratch = scratch_; retained_.geometry = session_->geometry();
        retained_.uuid = uuid_; retained_.targets = targets_; retained_.count = count_;
        retained_.journal_first = journal_first_; retained_.journal_blocks = journal_blocks_;
        retained_.journal_table = journal_table_; retained_.sequence = sequence_;
        retained_.ready = true; established_ = true;
    }
    [[nodiscard]] Status records_exact() const
    {
        if (records_sealed_ != retained_.records_ready)
            return Status::fault("reclaim record seal latch changed");
        if (!records_sealed_)
            return established_ || admitted_ ? Status::fault("reclaim established record seal rolled back") : Status{};
        if (!storage_ || storage_ != retained_.storage || count_ != retained_.count ||
            count_ > targets_.size() || uuid_ != retained_.uuid || targets_ != retained_.targets ||
            journal_first_ != retained_.journal_first || journal_blocks_ != retained_.journal_blocks ||
            journal_table_ != retained_.journal_table || sequence_ != retained_.sequence)
            return Status::fault("reclaim original record identity changed");
        const auto &s = *storage_;
        if (s.superblock != retained_.super || s.descriptors != retained_.descriptors ||
            s.journal_inode != retained_.journal_inode || s.head != retained_.head || s.descriptor != retained_.descriptor ||
            s.commit != retained_.commit || s.end != retained_.end || s.old_head != retained_.old_head ||
            s.normalized != retained_.normalized || s.parser_record != retained_.parser_record ||
            s.parser_payload != retained_.parser_payload || s.payloads != retained_.payloads ||
            s.before != retained_.before || s.after != retained_.after)
            return Status::fault("reclaim whole retained bootstrap or WAL changed");
        for (usize index = 0; index < s.actions.size(); ++index)
        {
            const auto &a = s.actions[index], &b = retained_.actions[index];
            if (a.target_block != b.target_block || a.journal_block != b.journal_block || a.sequence != b.sequence ||
                a.transaction_order != b.transaction_order || a.checksum != b.checksum || a.restore_magic != b.restore_magic)
                return Status::fault("reclaim retained replay action changed");
        }
        for (usize index = 0; index < s.revocations.size(); ++index)
            if (s.revocations[index].target_block != retained_.revocations[index].target_block ||
                s.revocations[index].transaction_order != retained_.revocations[index].transaction_order)
                return Status::fault("reclaim retained revocation changed");
        return {};
    }
    // No transport call and no read-only Session counter assumption here.
    // This interpretation remains sealed when a private Owner later writes.
    [[nodiscard]] Status retained_exact(bool include_capture = true) const
    {
        if (rejected_) return Status::io_error("reclaim retained binding is terminally rejected");
        if (!session_anchor_.exact(session_) || !session_anchor_.exact(original_session_))
            return Status::fault("reclaim original Session borrow anchors changed");
        if (auto status = records_exact(); !status) return status;
        if (!established_) return retained_.ready ? Status::fault("reclaim binding seal latch differs") : Status{};
        if (!attempted_ || !retained_.ready || retained_.terminal != admitted_ ||
            capture_admitted_ != retained_.capture_admitted || (admitted_ && !capture_admitted_) || session_ != retained_.session || storage_ != retained_.storage ||
            scratch_ != retained_.scratch || original_session_ != retained_.session ||
            !reclaim_history::same_session(original_io_, retained_.report.original) || !storage_ || !scratch_ || count_ != retained_.count ||
            count_ > targets_.size() || report_.bootstrap_observations > storage_->bootstrap_reads.size() ||
            uuid_ != retained_.uuid || targets_ != retained_.targets || journal_first_ != retained_.journal_first ||
            journal_blocks_ != retained_.journal_blocks || journal_table_ != retained_.journal_table ||
            sequence_ != retained_.sequence || !reclaim_history::same_info(binding_, retained_.binding) ||
            !bootstrap_.info() || !reclaim_history::same_info(*bootstrap_.info(), retained_.bootstrap_binding) ||
            !same_report(report_, retained_.report) ||
            (retained_.terminal && report_.phase != Ext4ReclaimRecoveryPhase::ready))
            return Status::fault("reclaim retained binding identity or interpretation changed");
        const auto geometry = session_->geometry();
        if (geometry.block_size != retained_.geometry.block_size || geometry.block_count != retained_.geometry.block_count ||
            geometry.writable != retained_.geometry.writable)
            return Status::io_error("reclaim retained original geometry changed");
        const auto &s = *storage_;
        for (usize index = 0; index < s.bootstrap_reads.size(); ++index)
            if (s.bootstrap_reads[index].physical != retained_.bootstrap[index].physical ||
                s.bootstrap_reads[index].bytes != retained_.bootstrap[index].bytes)
                return Status::fault("reclaim retained bootstrap observation changed");
        if (include_capture && retained_.capture_admitted &&
            (!reclaim_history::same_info(capture_.binding_, retained_.binding) ||
             !capture_.retained_exact() || !Ext4OrphanReclaimCapture<DeviceCapacity>::same_report(
                capture_.report(), retained_.capture_report)))
            return Status::fault("reclaim retained capture result changed");
        return {};
    }

    reclaim_history::SessionAnchor<Session> session_anchor_{};
    Session *session_{}, *original_session_{};
    block::SessionReport original_io_{};
    Ext4ReclaimRecoveryStorage *storage_{};
    Ext4OrphanReclaimColdScratch *scratch_{};
    Ext4ReclaimRecoveryReport report_{};
    Ext4RecoveryBinding bootstrap_{};
    Ext4OrphanReclaimCapture<DeviceCapacity> capture_{};
    Ext4RecoveryInfo binding_{};
    std::array<u8, 16> uuid_{};
    std::array<u32, 5> targets_{};
    usize count_{};
    u32 journal_first_{}, journal_blocks_{}, journal_table_{}, sequence_{};
    Retained retained_{};
    bool attempted_{}, established_{}, rejected_{}, capture_admitted_{}, admitted_{}, records_sealed_{};
};

// Private read-only projection. No report or projected byte grants write authority.
template <usize DeviceCapacity = 4>
class Ext4ReclaimResultHistory final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4OrphanChainCapture;
    template <usize> friend class Ext4ReclaimComposedHistory;
    using Binding = Ext4ReclaimRecoveryBinding<DeviceCapacity>;
    using Capture = Ext4OrphanReclaimCapture<DeviceCapacity>;
    using Session = typename block::BlockSessions<DeviceCapacity>::Session;
    using Block = Ext4OrphanBlock;
    using UnguardedRead = typename Capture::UnguardedRead;
    struct Domain
    {
        const void *owner{}, *cache{}, *workspace{}, *mount{};
        u64 epoch{}, generation{};
        Status (*guard)(const void *){};
        const void *context{};
    };
    struct Member { u32 inode{}, generation{}, next{}; };
    struct Seal
    {
        const Binding *binding{};
        Session *session{};
        Domain domain{};
        Ext4ReclaimRecoveryReport binding_report{};
        Ext4OrphanReclaimCaptureReport capture_report{};
        Block empty_head{};
        std::array<Member, 7> members{};
        block::SessionReport initial{}, progress{};
        usize count{};
        block::SessionCounter reads{};
        bool ready{}, reading{};
    };
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    class OrdinaryView final : public driver::BlockDevice
    {
      public:
        explicit OrdinaryView(Ext4ReclaimResultHistory &history, Guard &guard) : history_(history), guard_(guard) {}
        driver::BlockGeometry geometry() const override { return history_.original_session_->geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> bytes) override
        { return history_.read_blocks(first, bytes, guard_); }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("active reclaim prospective ordinary view is readonly"); }
        Status flush() override { return Status::denied("active reclaim prospective FLUSH is forbidden"); }
      private:
        Ext4ReclaimResultHistory &history_;
        Guard &guard_;
    };
  public:
    Ext4ReclaimResultHistory() = default;
    Ext4ReclaimResultHistory(const Ext4ReclaimResultHistory &) = delete;
    Ext4ReclaimResultHistory &operator=(const Ext4ReclaimResultHistory &) = delete;
    Ext4ReclaimResultHistory(Ext4ReclaimResultHistory &&) = delete;
    Ext4ReclaimResultHistory &operator=(Ext4ReclaimResultHistory &&) = delete;
  private:
    static bool same_domain(const Domain &a, const Domain &b)
    {
        return a.owner == b.owner && a.cache == b.cache && a.workspace == b.workspace && a.mount == b.mount &&
            a.epoch == b.epoch && a.generation == b.generation && a.guard == b.guard && a.context == b.context;
    }
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - 8 * index)); }
    [[nodiscard]] Status reject(Status status)
    {
        rejected_ = true;
        static_cast<void>(session_anchor_.abort(status));
        return status;
    }
    [[nodiscard]] Status prepare(const Binding &binding, Domain domain)
    {
        if (attempted_) return Status::already_exists("active reclaim result history is one-shot");
        attempted_ = true; binding_ = &binding; domain_ = domain;
        original_session_ = binding.original_session_;
        if (auto status = binding.retained_exact(); !status)
        { static_cast<void>(binding.session_anchor_.abort(status)); rejected_ = true; return status; }
        if (!original_session_) return Status::not_initialized("active reclaim original Session is absent");
        if (auto status = session_anchor_.bind(*original_session_); !status) return status;
        if (!domain.owner || !domain.cache || !domain.workspace || !domain.mount ||
            !domain.guard || domain.context != domain.owner || domain.epoch != 1 || domain.generation != 1)
            return reject(Status::fault("active reclaim original opening domain is absent"));
        if (auto status = domain.guard(domain.context); !status) return reject(status);
        if (auto status = binding.retained_exact(); !status) return reject(status);
        if (!binding.admitted_ || !binding.retained_.terminal || !binding.capture_.admitted_ || !original_session_)
            return reject(Status::not_initialized("active reclaim result requires original admitted capture"));
        const auto &capture = binding.capture_.retained_;
        const auto &proof = capture.report.semantic;
        const auto &plan = capture.plan.reclaim;
        if (!proof.valid || !proof.complete_global_before_proof || !capture.plan.valid || !capture.journal.ready ||
            proof.count < 4 || proof.count > 5 || plan.count != proof.count || plan.inode != proof.inode ||
            plan.next_head != proof.next_head || proof.active_sequence > ~u32{0} - 2 ||
            proof.parsed_next_sequence != proof.active_sequence + 1 || proof.cleanup_sequence != proof.active_sequence + 2)
            return reject(Status::fault("active reclaim original complete result is inconsistent"));
        empty_head_ = capture.journal.journal_head;
        big32(empty_head_, 24, proof.cleanup_sequence); big32(empty_head_, 28, 0); big32(empty_head_, 252, 0);
        big32(empty_head_, 252, jbd2::detail::crc32c(~u32{0}, empty_head_));
        const auto codec = jbd2::Codec::open(empty_head_, binding.journal_blocks_, binding.uuid_,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec || codec.value().superblock().sequence != proof.cleanup_sequence || codec.value().superblock().start_block)
            return reject(Status::fault("active reclaim prospective empty head differs"));
        const auto initial = original_session_->report(); if (!initial) return reject(initial.status());
        initial_ = progress_ = initial.value();
        if (!reclaim_history::same_session(progress_, binding.report_.observed) || progress_.in_flight ||
            progress_.pending_writes || progress_.state != block::DeviceState::owned ||
            progress_.first_error != StatusCode::ok || progress_.reads != progress_.completed_reads ||
            progress_.writes != progress_.completed_writes || progress_.flushes != progress_.completed_flushes)
            return reject(Status::fault("active reclaim prospective original Session differs"));
        // Derive remaining identities from retained whole terminal tables, never current verifier scratch.
        u32 number = proof.next_head;
        while (number)
        {
            if (number < 11 || number > 8192 || number == proof.inode || member_count_ == members_.size())
                return reject(Status::unsupported("active reclaim prospective remaining chain capacity"));
            for (usize index = 0; index < member_count_; ++index)
                if (members_[index].inode == number) return reject(Status::fault("active reclaim prospective remaining chain cycle"));
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (!capture.initialized[group]) return reject(Status::fault("active reclaim prospective member table is absent"));
            const Block *table{};
            if (auto status = terminal(u32(capture.binding.metadata[group * 4 + 3].first) + local / 4, table); !status)
                return reject(status);
            const auto inode = std::span<const u8>{*table}.subspan((local % 4) * 256, 256);
            members_[member_count_++] = {number, ext4_checked::le32(inode, 100), ext4_checked::le32(inode, 20)};
            number = members_[member_count_ - 1].next;
        }
        seal_.binding = binding_; seal_.session = original_session_; seal_.domain = domain_;
        seal_.binding_report = binding.report_; seal_.capture_report = binding.capture_.report_;
        seal_.empty_head = empty_head_; seal_.members = members_; seal_.count = member_count_;
        seal_.initial = seal_.progress = progress_; seal_.ready = true; ready_ = true;
        if (auto status = stable(); !status) return reject(status);
        // Every duplicate physical observation is reconciled before the first projected read.
        if (auto status = each_original([&](u32 physical, const Block &image) -> Status {
                const Block *expected{};
                if (auto checked = original(physical, expected); !checked) return checked;
                return *expected == image ? Status{} : Status::fault("active reclaim duplicate original observation differs");
            }); !status) return reject(status);
        return stable();
    }
    // Pure retained guard; it deliberately does not assume the read-only binding's Session counters still apply.
    [[nodiscard]] Status retained_exact() const
    {
        if (!session_anchor_.exact(original_session_) || rejected_ || !attempted_ || !ready_ || !seal_.ready || binding_ != seal_.binding ||
            original_session_ != seal_.session || !binding_ || !original_session_ || original_session_ != binding_->original_session_ ||
            reading_ != seal_.reading || !same_domain(domain_, seal_.domain) ||
            empty_head_ != seal_.empty_head || member_count_ != seal_.count || member_count_ > members_.size() ||
            reads_ != seal_.reads || !reclaim_history::same_session(initial_, seal_.initial) ||
            !reclaim_history::same_session(progress_, seal_.progress))
            return Status::fault("active reclaim prospective original identity or progress changed");
        if (initial_.reads > ~block::SessionCounter{0} - reads_) return Status::overflow("active reclaim prospective cumulative read count");
        auto expected_io = initial_; expected_io.reads += reads_; expected_io.completed_reads += reads_;
        if (!reclaim_history::same_session(progress_, expected_io))
            return Status::fault("active reclaim prospective original exact read equation differs");
        for (usize index = 0; index < members_.size(); ++index)
            if (members_[index].inode != seal_.members[index].inode || members_[index].generation != seal_.members[index].generation ||
                members_[index].next != seal_.members[index].next)
                return Status::fault("active reclaim prospective complete member vector changed");
        if (auto status = binding_->retained_exact(); !status) return status;
        if (!Binding::same_report(binding_->report_, seal_.binding_report) ||
            !Capture::same_report(binding_->capture_.report_, seal_.capture_report))
            return Status::fault("active reclaim original readonly reports changed");
        return domain_.guard(domain_.context);
    }
    [[nodiscard]] Status stable() const
    {
        if (auto status = retained_exact(); !status) return status;
        const auto current = original_session_->report(); if (!current) return current.status();
        return reclaim_history::same_session(current.value(), progress_) ? Status{} :
            Status::fault("active reclaim prospective exact Session progress differs");
    }
    [[nodiscard]] Status in_flight_exact() const
    {
        if (auto status = retained_exact(); !status) return status;
        if (!reading_ || progress_.reads == ~block::SessionCounter{0}) return Status::fault("active reclaim prospective read is not in flight");
        auto expected = progress_; ++expected.reads; expected.in_flight = true;
        const auto current = original_session_->report(); if (!current) return current.status();
        return reclaim_history::same_session(current.value(), expected) ? Status{} :
            Status::fault("active reclaim prospective exact nested in-flight tuple differs");
    }
    [[nodiscard]] Status original(u32 physical, const Block *&out) const
    {
        out = nullptr;
        if (!binding_ || physical >= 32768) return Status::invalid_argument("active reclaim prospective physical bounds");
        Status result;
        const auto observe = [&](const Block &image) {
            if (out && *out != image) result = Status::fault("active reclaim overlapping original whole bytes differ");
            else if (!out) out = &image;
        };
        const auto &outer = binding_->retained_;
        const auto &capture = binding_->capture_.retained_; const auto &storage = capture.storage;
        if (physical == 1) { observe(outer.super); observe(storage.superblock); }
        if (physical == 2) { observe(outer.descriptors); observe(storage.descriptors); }
        if (physical == binding_->journal_table_) observe(outer.journal_inode);
        const auto first = binding_->journal_first_;
        if (physical == first) { observe(outer.head); observe(storage.head); }
        if (physical == first + 1) { observe(outer.descriptor); observe(storage.descriptor); }
        for (usize index = 0; index < capture.count; ++index)
        {
            if (physical == first + u32(index) + 2) { observe(outer.payloads[index]); observe(storage.payloads[index]); }
            if (physical == capture.plan.reclaim.targets[index]) observe(capture.current[index]);
        }
        if (physical == first + u32(capture.count) + 2) { observe(outer.commit); observe(storage.commit); }
        if (physical == first + u32(capture.count) + 3) { observe(outer.end); observe(storage.end); }
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = capture.binding.metadata.data() + group * 4;
            if (physical == metadata[1].first) observe(storage.block_bitmaps[group]);
            if (physical == metadata[2].first) observe(storage.inode_bitmaps[group]);
            if (capture.initialized[group] && physical >= metadata[3].first && physical - metadata[3].first < 512)
                observe(storage.tables[group][physical - metadata[3].first]);
        }
        for (usize index = 0; index < capture.directories; ++index)
            if (physical == storage.directories[index].physical) observe(storage.directories[index].bytes);
        for (usize index = 0; index < outer.report.bootstrap_observations; ++index)
            if (physical == outer.bootstrap[index].physical) observe(outer.bootstrap[index].bytes);
        if (!result) return result;
        return out ? Status{} : Status::unsupported("active reclaim prospective unknown original block");
    }
    [[nodiscard]] Status terminal(u32 physical, const Block *&out) const
    {
        if (auto status = original(physical, out); !status) return status;
        if (physical == binding_->journal_first_) out = &empty_head_;
        else
        {
            const auto &plan = binding_->capture_.retained_.plan.reclaim;
            for (usize index = 0; index < plan.count; ++index)
                if (physical == plan.targets[index]) out = &plan.after[index];
        }
        return {};
    }
    template <typename Visitor> [[nodiscard]] Status each_original(Visitor visit) const
    {
        const auto &outer = binding_->retained_; const auto &capture = binding_->capture_.retained_;
        const auto &storage = capture.storage;
        if (auto status = visit(1, storage.superblock); !status) return status;
        if (auto status = visit(2, storage.descriptors); !status) return status;
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = capture.binding.metadata.data() + group * 4;
            if (auto status = visit(u32(metadata[1].first), storage.block_bitmaps[group]); !status) return status;
            if (auto status = visit(u32(metadata[2].first), storage.inode_bitmaps[group]); !status) return status;
            if (capture.initialized[group])
                for (usize index = 0; index < 512; ++index)
                    if (auto status = visit(u32(metadata[3].first + index), storage.tables[group][index]); !status) return status;
        }
        for (usize index = 0; index < capture.directories; ++index)
            if (auto status = visit(storage.directories[index].physical, storage.directories[index].bytes); !status) return status;
        for (usize index = 0; index < outer.report.bootstrap_observations; ++index)
            if (auto status = visit(outer.bootstrap[index].physical, outer.bootstrap[index].bytes); !status) return status;
        for (usize index = 0; index < capture.count + 4; ++index)
        {
            const u32 physical = binding_->journal_first_ + u32(index); const Block *expected{};
            if (auto status = original(physical, expected); !status) return status;
            if (auto status = visit(physical, *expected); !status) return status;
        }
        return {};
    }
    [[nodiscard]] Status read_blocks(u64 first, std::span<std::byte> bytes)
    {
        UnguardedRead guard;
        return read_blocks(first, bytes, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status read_blocks(u64 first, std::span<std::byte> bytes, Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        if (first % 2 || first >= 65536 || bytes.size() != 1024 || progress_.reads == ~block::SessionCounter{0} || reads_ == ~block::SessionCounter{0})
            return reject(Status::overflow("active reclaim prospective read bounds or counter"));
        const Block *before{}, *after{}; const u32 physical = u32(first / 2);
        if (auto status = original(physical, before); !status) return reject(status);
        if (auto status = terminal(physical, after); !status) return reject(status);
        if (auto status = transport_read(first, bytes, before, guard); !status) return status;
        for (usize at = 0; at < 1024; ++at) bytes[at] = std::byte((*after)[at]);
        return stable();
    }
    // Only the composed route can observe the explicitly named ninth WAL block.
    // These methods preserve the original interpretation and readonly reports.
    [[nodiscard]] Status extension_read(std::span<std::byte> bytes, const Block *expected)
    {
        UnguardedRead guard;
        return extension_read(bytes, expected, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status extension_read(std::span<std::byte> bytes, const Block *expected, Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        const u64 physical = u64{binding_->journal_first_} + 8;
        if (binding_->capture_.retained_.count != 4 || binding_->journal_blocks_ <= 8 || physical >= 32768 ||
            bytes.size() != 1024 || progress_.reads == ~block::SessionCounter{0} || reads_ == ~block::SessionCounter{0})
            return reject(Status::unsupported("composed original ninth WAL source geometry differs"));
        return transport_read(physical * 2, bytes, expected, guard);
    }
    [[nodiscard]] Status transport_read(u64 first, std::span<std::byte> bytes, const Block *before)
    {
        UnguardedRead guard;
        return transport_read(first, bytes, before, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status transport_read(u64 first, std::span<std::byte> bytes, const Block *before, Guard &guard)
    {
        const auto previous = progress_; const auto previous_reads = reads_;
        reading_ = seal_.reading = true;
        auto *original = original_session_;
        const auto status = block::guarded_read_blocks(*original, guard, first, bytes);
        if (!status) return reject(status);
        if (auto guarded = retained_exact(); !guarded) return reject(guarded);
        if (!reading_ || reads_ != previous_reads || !reclaim_history::same_session(progress_, previous))
            return reject(Status::fault("active reclaim callback changed paused progress"));
        auto expected = previous; ++expected.reads; ++expected.completed_reads;
        const auto actual = original_session_->report();
        if (!actual || !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("active reclaim callback Session completion differs"));
        if (before)
            for (usize at = 0; at < 1024; ++at)
                if (bytes[at] != std::byte((*before)[at]))
                    return reject(Status::io_error("active reclaim original whole physical bytes changed"));
        reading_ = seal_.reading = false;
        progress_ = seal_.progress = expected; ++reads_; ++seal_.reads;
        return stable();
    }
    [[nodiscard]] Status compare_all()
    {
        UnguardedRead guard;
        return compare_all(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_all(Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        const auto result = each_original([&](u32 physical, const Block &) -> Status {
            const Block *expected{};
            if (auto status = terminal(physical, expected); !status) return status;
            if (auto status = read_blocks(u64{physical} * 2, std::as_writable_bytes(std::span{checking_}), guard); !status) return status;
            return checking_ == *expected ? Status{} : Status::fault("active reclaim prospective complete terminal history differs");
        });
        if (!result) return reject(result);
        return stable();
    }
    [[nodiscard]] Status prove_ordinary(Ext4RecoveryBinding &ordinary)
    {
        UnguardedRead guard;
        return prove_ordinary(ordinary, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status prove_ordinary(Ext4RecoveryBinding &ordinary, Guard &guard)
    {
        if (member_count_ || binding_->capture_.retained_.report.semantic.next_head)
            return reject(Status::denied("active reclaim zero-head proof requires no remaining members"));
        OrdinaryView<Guard> view(*this, guard);
        if (auto status = ordinary.inspect(view, {0, original_session_->geometry().block_count}); !status) return reject(status);
        const auto *info = ordinary.info();
        auto expected = binding_->capture_.retained_.binding;
        expected.empty_log = true; expected.journal_sequence = binding_->capture_.retained_.report.semantic.cleanup_sequence;
        if (!info || !reclaim_history::same_info(*info, expected))
            return reject(Status::fault("active reclaim prospective ordinary terminal binding differs"));
        if (auto status = compare_all(guard); !status) return status;
        if (!ordinary.info() || !reclaim_history::same_info(*ordinary.info(), expected))
            return reject(Status::fault("active reclaim prospective ordinary result changed during terminal reads"));
        return stable();
    }
    [[nodiscard]] Status validate_remaining(const Ext4RecoveryInfo &info, const Ext4OrphanDirectoryReferenceSource &source)
    {
        UnguardedRead guard;
        return validate_remaining(info, source, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status validate_remaining(const Ext4RecoveryInfo &info, const Ext4OrphanDirectoryReferenceSource &source, Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        auto expected = binding_->capture_.retained_.binding;
        expected.empty_log = true; expected.journal_sequence = binding_->capture_.retained_.report.semantic.cleanup_sequence;
        if (!member_count_ || !reclaim_history::same_info(info, expected))
            return reject(Status::fault("active reclaim prospective remaining binding differs"));
        Ext4OrphanChainProof chain{};
        if (auto status = Ext4OrphanChainValidator::validate(info, source.ownership.chain, chain); !status) return reject(status);
        if (chain.count() != member_count_ || expected.journal_sequence > ~u32{0} - u32(3 * member_count_))
            return reject(Status::overflow("active reclaim prospective complete sequence or member reservation"));
        for (usize index = 0; index < member_count_; ++index)
        {
            const auto *entry = chain.at(index); const auto &identity = members_[index];
            if (!entry || entry->number != identity.inode || entry->generation != identity.generation || entry->next != identity.next)
                return reject(Status::fault("active reclaim prospective remaining identity differs"));
            if (entry->generation == ~u32{0}) return reject(Status::overflow("active reclaim remaining generation exhausted"));
            const auto &image = source.ownership.chain.chain[index];
            const auto inode = std::span<const u8>{image.table}.subspan(((image.number - 1) % 4) * 256, 256);
            const u32 seconds = ext4_checked::le32(inode, 12), extra = ext4_checked::le32(inode, 132);
            if (seconds > 0x7fffffffU || (extra & 3) || (extra >> 2) >= 1000000000U)
                return reject(Status::unsupported("active reclaim remaining ctime outside writable profile"));
            const u32 group = entry->data_count ? (entry->data_blocks[0] - 1) / 8192 : 0;
            for (usize data = 0; data < entry->data_count; ++data)
                if ((entry->data_blocks[data] - 1) / 8192 != group)
                    return reject(Status::unsupported("active reclaim remaining data spans writable allocation groups"));
        }
        return compare_all(guard);
    }
    reclaim_history::SessionAnchor<Session> session_anchor_{};
    const Binding *binding_{};
    Session *original_session_{};
    Domain domain_{};
    Block empty_head_{}, checking_{};
    std::array<Member, 7> members_{};
    block::SessionReport initial_{}, progress_{};
    usize member_count_{};
    block::SessionCounter reads_{};
    Seal seal_{};
    bool attempted_{}, ready_{}, rejected_{}, reading_{};
};

template <usize> class Ext4ReclaimComposedHistory;
enum class Ext4ReclaimDryPhase : u8 { capture, plan, ordinary, terminal, unused };

// Diagnostic bounds for the concrete read-only route; never a write token.
struct Ext4ReclaimComposedReadCapacity
{
    std::array<block::SessionCounter, 4> per_pass{}, cumulative{};
    block::SessionCounter extension{}, aggregate{};
    usize remaining{};
    u32 projected_sequence_end{};
};

class Ext4ReclaimComposedReadBudget final
{
    template <usize> friend class Ext4ReclaimComposedHistory;
  public:
    Ext4ReclaimComposedReadBudget() = default;
    Ext4ReclaimComposedReadBudget(const Ext4ReclaimComposedReadBudget &) = delete;
    Ext4ReclaimComposedReadBudget &operator=(const Ext4ReclaimComposedReadBudget &) = delete;
    Ext4ReclaimComposedReadBudget(Ext4ReclaimComposedReadBudget &&) = delete;
    Ext4ReclaimComposedReadBudget &operator=(Ext4ReclaimComposedReadBudget &&) = delete;

    [[nodiscard]] static Result<Ext4ReclaimComposedReadCapacity> capacity(
        const block::SessionReport &initial, usize remaining, bool extension, u32 sequence)
    {
        if (remaining > 7) return Status::unsupported("composed dry remaining-member capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("composed dry reservation requires stable original counters");
        Ext4ReclaimComposedReadCapacity result;
        result.remaining = remaining; result.extension = extension ? 1 : 0;
        result.per_pass = {4654, 2327, 10163275, 2335};
        result.cumulative = result.per_pass;
        for (usize phase = 0; phase < 2; ++phase)
        {
            if (remaining && result.per_pass[phase] > ~block::SessionCounter{0} / remaining)
                return Status::overflow("composed dry stage reservation multiplication");
            result.cumulative[phase] = result.per_pass[phase] * remaining;
        }
        result.aggregate = result.extension;
        for (block::SessionCounter count : result.cumulative)
        {
            if (result.aggregate > ~block::SessionCounter{0} - count)
                return Status::overflow("composed dry aggregate reservation addition");
            result.aggregate += count;
        }
        if (initial.reads > ~block::SessionCounter{0} - result.aggregate ||
            initial.completed_reads > ~block::SessionCounter{0} - result.aggregate)
            return Status::overflow("composed complete dry READ reservation exhausted");
        if (remaining > (~u32{0} - 2) / 3 || sequence > ~u32{0} - 2 - u32(3 * remaining))
            return Status::overflow("composed dry projected sequence reservation exhausted");
        result.projected_sequence_end = sequence + 2 + u32(3 * remaining);
        return result;
    }

  private:
    static bool same_capacity(const Ext4ReclaimComposedReadCapacity &a,
                              const Ext4ReclaimComposedReadCapacity &b)
    {
        return a.per_pass == b.per_pass && a.cumulative == b.cumulative &&
            a.extension == b.extension && a.aggregate == b.aggregate &&
            a.remaining == b.remaining && a.projected_sequence_end == b.projected_sequence_end;
    }
    [[nodiscard]] Status reserve(const block::SessionReport &initial, usize remaining,
                                 bool extension, u32 sequence)
    {
        if (attempted_) return Status::already_exists("composed dry reservation is one-shot");
        attempted_ = true;
        const auto result = capacity(initial, remaining, extension, sequence);
        if (!result) return result.status();
        capacity_ = sealed_ = result.value(); ready_ = true;
        return {};
    }
    [[nodiscard]] Status exact(const block::SessionReport &initial, usize remaining,
                               bool extension, u32 sequence) const
    {
        if (!attempted_ || !ready_ || !same_capacity(capacity_, sealed_))
            return Status::fault("composed sealed dry reservation changed");
        const auto expected = capacity(initial, remaining, extension, sequence);
        if (!expected) return expected.status();
        return same_capacity(capacity_, expected.value()) ? Status{} :
            Status::fault("composed dry reservation differs from original route facts");
    }
    [[nodiscard]] block::SessionCounter per_pass(Ext4ReclaimDryPhase phase) const
    { return usize(phase) < capacity_.per_pass.size() ? capacity_.per_pass[usize(phase)] : 0; }
    [[nodiscard]] block::SessionCounter cumulative(Ext4ReclaimDryPhase phase) const
    { return usize(phase) < capacity_.cumulative.size() ? capacity_.cumulative[usize(phase)] : 0; }
    [[nodiscard]] Status charges(block::SessionCounter extension, const std::array<block::SessionCounter, 4> &stages, block::SessionCounter reads) const
    {
        if (!ready_ || extension > capacity_.extension)
            return Status::overflow("composed dry extension reservation exhausted");
        block::SessionCounter aggregate = extension;
        for (usize phase = 0; phase < stages.size(); ++phase)
        {
            if (stages[phase] > capacity_.cumulative[phase] || aggregate > ~block::SessionCounter{0} - stages[phase])
                return Status::overflow("composed dry stage reservation exhausted");
            aggregate += stages[phase];
        }
        if (aggregate > capacity_.aggregate)
            return Status::overflow("composed dry aggregate reservation exhausted");
        return aggregate == reads ? Status{} : Status::fault("composed dry reserved charges differ from Session reads");
    }
    Ext4ReclaimComposedReadCapacity capacity_{}, sealed_{};
    bool attempted_{}, ready_{};
};

template <usize, usize> class Ext4OwnedTransaction;

enum class Ext4ReclaimRoutePhase : u8 { unused, opening, refined, handoff, closed, failed };
enum class Ext4ReclaimRouteOperation : u8 { read, write, flush, none };

// Public copies carry no authority and cannot admit a writer phase.
struct Ext4ReclaimRouteCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    usize remaining{};
    u32 projected_sequence_end{};
};
struct Ext4ReclaimRouteBudgetReport
{
    Ext4ReclaimRoutePhase phase{Ext4ReclaimRoutePhase::unused};
    Ext4ReclaimRouteCapacity capacity{};
    std::array<block::SessionCounter, 3> attempts{}, completions{};
    block::SessionReport observed{};
    bool attempted{}, refined{}, dispatch_pending{};
};

// The exact original Session and its service outlive this serialized guard.
// Pair each successful begin with one actual Session operation and one finish.
template <typename Session> class Ext4ReclaimRouteBudget final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Phase = Ext4ReclaimRoutePhase;
    using Operation = Ext4ReclaimRouteOperation;
  public:
    Ext4ReclaimRouteBudget() = default;
    ~Ext4ReclaimRouteBudget()
    {
        if (control_.operation != Operation::none || sealed_.operation != Operation::none)
            static_cast<void>(anchor_.abort(Status::interrupted("reclaim route unfinished dispatch")));
    }
    Ext4ReclaimRouteBudget(const Ext4ReclaimRouteBudget &) = delete;
    Ext4ReclaimRouteBudget &operator=(const Ext4ReclaimRouteBudget &) = delete;
    Ext4ReclaimRouteBudget(Ext4ReclaimRouteBudget &&) = delete;
    Ext4ReclaimRouteBudget &operator=(Ext4ReclaimRouteBudget &&) = delete;

    template <typename Counter = block::SessionCounter> requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Result<Ext4ReclaimRouteCapacity> capacity(
        const block::SessionReport &initial, usize remaining, u32 sequence)
    {
        if (remaining > 7) return Status::unsupported("reclaim route remaining-member capacity");
        if (!stable(initial)) return Status::fault("reclaim route capacity requires stable Session");
        const u32 delta = 2 + u32(3 * remaining);
        if (sequence > ~u32{0} - delta)
            return Status::overflow("reclaim route projected sequence exhausted");
        const Ext4ReclaimRouteCapacity result{read_caps_[remaining], 6 + 15 * remaining,
                                              5 + 8 * remaining, remaining, sequence + delta};
        const auto attempts = attempted_counts(initial), completions = completed_counts(initial);
        const std::array<block::SessionCounter, 3> caps{result.reads, result.writes, result.flushes};
        constexpr u64 limit = u64(Counter(~Counter{0}));
        for (usize index = 0; index < caps.size(); ++index)
            if (u64(caps[index]) > limit || u64(attempts[index]) > limit - u64(caps[index]) ||
                u64(completions[index]) > limit - u64(caps[index]))
                return Status::overflow("reclaim route whole attempt/completion capacity exhausted");
        return result;
    }

    [[nodiscard]] Status reserve_opening(Session &session)
    {
        if (control_.operation != Operation::none || sealed_.operation != Operation::none)
            return reject(Status::fault("reclaim route reservation reentered dispatch"));
        if (attempted_) return Status::already_exists("reclaim route reservation is one-shot");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return status;
        original_ = &session;
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        // Reserve all maximum counters before classifier READs; no WRITE grant.
        const auto available = capacity(actual.value(), 7, 0);
        if (!available) return refuse(available.status());
        const auto geometry = session.geometry();
        if (geometry.block_size != 512)
            return refuse(Status::unsupported("reclaim route requires 512-byte sectors"));
        if (!geometry.block_count || !geometry.block_size)
            return reject(Status::fault("reclaim route original Session geometry"));
        const auto bounds = session.bounds();
        if (!valid_bounds(bounds, geometry))
            return reject(Status::fault("reclaim route original Session volume bounds"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = observed_ = actual.value();
        geometry_ = geometry_seal_ = geometry;
        bounds_ = bounds_seal_ = bounds;
        control_.phase = Phase::opening;
        sync();
        return {};
    }

    [[nodiscard]] Status refine(Session &session, usize remaining, u32 active_sequence)
    {
        if (control_.operation != Operation::none)
            return reject(Status::fault("reclaim route refinement reentered dispatch"));
        if (refine_attempted_) return Status::already_exists("reclaim route refinement is one-shot");
        if (auto status = idle_exact(session); !status) return status;
        if (control_.phase != Phase::opening)
            return reject(Status::fault("reclaim route refinement outside opening"));
        refine_attempted_ = true;
        if (!active_sequence)
            return refuse(Status::invalid_argument("reclaim route active sequence must be nonzero"));
        const auto narrowed = capacity(initial_, remaining, active_sequence);
        if (!narrowed) return refuse(narrowed.status());
        if (control_.attempts[0] > narrowed.value().reads ||
            control_.completions[0] > narrowed.value().reads)
            return refuse(Status::overflow("reclaim route opening reads exceed refined capacity"));
        control_.remaining = remaining;
        control_.sequence = active_sequence;
        control_.projected_sequence_end = narrowed.value().projected_sequence_end;
        control_.phase = Phase::refined;
        sync();
        return {};
    }

    [[nodiscard]] Status begin_read(Session &session, u64 first_sector, usize transfer_bytes = 1024)
    { return begin(session, Operation::read, first_sector, transfer_bytes); }
    [[nodiscard]] Status finish_read(Session &session, Status transport)
    { return finish(session, Operation::read, transport); }

    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation == Operation::none)
            return reject(Status::fault("reclaim route callback retained authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_))
        {
            // Keep the independent selected dispatch intact until finish
            // accounts the failed Session attempt. Quarantine prevents any
            // subsequent transport and the Session cannot credit completion.
            const auto status = Status::fault("reclaim route callback original volume changed");
            static_cast<void>(anchor_.abort(status));
            return status;
        }
        auto expected = flight_;
        advance_attempt(expected, control_.operation);
        expected.in_flight = true;
        if (control_.operation == Operation::write) expected.pending_writes = true;
        return reclaim_history::same_session(actual.value(), expected) ? Status{} :
            reject(Status::fault("reclaim route callback Session prefix changed"));
    }

    [[nodiscard]] Status close(Session &session)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (progress_.pending_writes)
            return reject(Status::busy("reclaim route close has unresolved writes"));
        control_.phase = Phase::closed;
        control_.writer = false;
        sync();
        return {};
    }

    [[nodiscard]] Ext4ReclaimRouteBudgetReport report() const
    {
        return {control_.phase, limits(), control_.attempts, control_.completions, observed_,
                attempted_, refine_attempted_, control_.operation != Operation::none};
    }

  private:
    // Only the continuing Owner records this identity before classifier I/O.
    // A public reserve_opening call deliberately cannot acquire this attachment.
    [[nodiscard]] Status reserve_owned_opening(Session &session, const void *owner)
    {
        if (!owner || owner_ || owner_seal_)
            return reject(Status::fault("reclaim route original Owner reservation differs"));
        if (auto status = reserve_opening(session); !status) return status;
        owner_ = owner_seal_ = owner;
        return {};
    }
    // Detach the opening view at quiescence without closing/replacing the budget.
    // This is accounting handoff, not admission of canonical publication or I/O.
    [[nodiscard]] Status quiescent_handoff(Session &session, const void *owner)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (!owner || owner != owner_ || owner_ != owner_seal_ || handoff_attempted_ ||
            control_.phase != Phase::refined || control_.writer || progress_.pending_writes)
            return reject(Status::fault("reclaim route same-Owner handoff unavailable"));
        handoff_attempted_ = true;
        control_.phase = Phase::handoff;
        sync();
        return {};
    }
    // The Owner may account a completed, independently guarded read-only
    // subroute. This never grants WRITE/FLUSH authority and must be called
    // before the Owner adopts the subroute's Session progress shadow.
    [[nodiscard]] Status adopt_verified_reads(Session &session,
                                               const block::SessionReport &before, block::SessionCounter count)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != Operation::none ||
            !reclaim_history::same_session(before, progress_))
            return reject(Status::fault("reclaim route guarded subroute origin differs"));
        const auto bound = limits().reads;
        if (count > bound - control_.attempts[0] || count > bound - control_.completions[0] ||
            count > ~block::SessionCounter{0} - progress_.reads || count > ~block::SessionCounter{0} - progress_.completed_reads)
            return reject(Status::overflow("reclaim route guarded subroute read credit exhausted"));
        auto expected = progress_;
        expected.reads += count; expected.completed_reads += count;
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("reclaim route guarded subroute Session prefix differs"));
        control_.attempts[0] += count; control_.completions[0] += count;
        progress_ = progress_seal_ = actual.value();
        sync();
        return {};
    }

    struct Control
    {
        Phase phase{Phase::unused};
        Operation operation{Operation::none};
        std::array<block::SessionCounter, 3> attempts{}, completions{};
        usize remaining{}, transfer_bytes{};
        u64 first_sector{};
        u32 sequence{}, projected_sequence_end{};
        bool writer{};
    };
    // Full leaf transport route, independently derived with K=2327,
    // H=2335 and O=10163275. Classifier5 + binding4685 + result(2347 or
    // 6989) + composed(1+6981*n+O+H) + physical C1(6981*n) + canonical
    // misses(6*(n+1)) + whole comparisons((7+6*n)*H) + ordinary inspect O
    // + mounted view294 + stored metadata2326 + final journal1.
    // Source-label lookup and cache hits charge no transport READs.
    static constexpr std::array<block::SessionCounter, 8> read_caps_{
        20354895, 20387515, 20415493, 20443471, 20471449, 20499427, 20527405, 20555383};

    static bool stable(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    static std::array<block::SessionCounter, 3> attempted_counts(const block::SessionReport &value)
    { return {value.reads, value.writes, value.flushes}; }
    static std::array<block::SessionCounter, 3> completed_counts(const block::SessionReport &value)
    { return {value.completed_reads, value.completed_writes, value.completed_flushes}; }
    static bool same_geometry(const driver::BlockGeometry &a, const driver::BlockGeometry &b)
    { return a.block_count == b.block_count && a.block_size == b.block_size && a.writable == b.writable; }
    static bool same_bounds(const block::SessionBounds &a, const block::SessionBounds &b)
    { return a.first == b.first && a.count == b.count; }
    static bool valid_bounds(const block::SessionBounds &bounds, const driver::BlockGeometry &geometry)
    { return bounds.count && bounds.count == geometry.block_count && bounds.first <= ~u64{0} - bounds.count; }
    static bool same_control(const Control &a, const Control &b)
    {
        return a.phase == b.phase && a.operation == b.operation && a.attempts == b.attempts &&
            a.completions == b.completions && a.remaining == b.remaining &&
            a.transfer_bytes == b.transfer_bytes && a.first_sector == b.first_sector &&
            a.sequence == b.sequence && a.projected_sequence_end == b.projected_sequence_end &&
            a.writer == b.writer;
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Ext4ReclaimRouteCapacity limits() const
    {
        if (control_.phase == Phase::opening) return {read_caps_[7], 111, 61, 7, 0};
        if (control_.remaining > 7) return {};
        return {read_caps_[control_.remaining], 6 + 15 * control_.remaining,
                5 + 8 * control_.remaining, control_.remaining, control_.projected_sequence_end};
    }
    bool retained_exact() const
    {
        if (!attempted_ || !original_ || !anchor_.exact(original_) ||
            (control_.phase != Phase::opening && control_.phase != Phase::refined && control_.phase != Phase::handoff) ||
            owner_ != owner_seal_ ||
            (control_.writer && control_.phase != Phase::handoff) ||
            (handoff_attempted_ != (control_.phase == Phase::handoff)) ||
            (handoff_attempted_ && !owner_) ||
            !same_control(control_, sealed_) ||
            (control_.operation != Operation::none && !same_control(control_, flight_control_)) ||
            !reclaim_history::same_session(initial_, initial_seal_) ||
            !reclaim_history::same_session(progress_, progress_seal_) ||
            !reclaim_history::same_session(flight_, flight_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            !valid_bounds(bounds_, geometry_) || !stable(initial_)) return false;
        if (control_.phase == Phase::opening &&
            (refine_attempted_ || control_.writer || control_.remaining || control_.sequence ||
             control_.projected_sequence_end)) return false;
        const auto caps = control_.phase == Phase::opening ? capacity(initial_, 7, 0) :
            capacity(initial_, control_.remaining, control_.sequence);
        if (!caps || (control_.phase != Phase::opening &&
            (!refine_attempted_ || caps.value().projected_sequence_end != control_.projected_sequence_end))) return false;
        const auto bound = limits();
        const std::array<block::SessionCounter, 3> bounds{bound.reads, bound.writes, bound.flushes};
        const auto initial_attempts = attempted_counts(initial_), initial_completed = completed_counts(initial_);
        const auto progress_attempts = attempted_counts(progress_), progress_completed = completed_counts(progress_);
        for (usize index = 0; index < bounds.size(); ++index)
            if (control_.attempts[index] > bounds[index] || control_.completions[index] != control_.attempts[index] ||
                initial_attempts[index] > ~block::SessionCounter{0} - control_.attempts[index] ||
                initial_completed[index] > ~block::SessionCounter{0} - control_.completions[index] ||
                progress_attempts[index] != initial_attempts[index] + control_.attempts[index] ||
                progress_completed[index] != initial_completed[index] + control_.completions[index]) return false;
        if (progress_.identity != initial_.identity ||
            progress_.registration_generation != initial_.registration_generation ||
            progress_.session_serial != initial_.session_serial || progress_.state != block::DeviceState::owned ||
            progress_.first_error != StatusCode::ok || progress_.in_flight) return false;
        return control_.writer || (!progress_.pending_writes && !control_.attempts[1] && !control_.attempts[2]);
    }

    [[nodiscard]] Status refuse(Status status)
    {
        attempted_ = true;
        control_.phase = Phase::failed;
        control_.writer = false;
        sync();
        return status;
    }
    [[nodiscard]] Status reject(Status status)
    {
        static_cast<void>(anchor_.abort(status));
        if (anchor_.exact(original_))
        {
            const auto actual = original_->report();
            if (actual) observed_ = actual.value();
        }
        return refuse(status);
    }
    [[nodiscard]] Status idle_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != Operation::none)
            return reject(Status::fault("reclaim route original quiescent authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        if (!reclaim_history::same_session(actual.value(), progress_) ||
            !same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_))
            return reject(Status::fault("reclaim route quiescent Session progress changed"));
        return {};
    }

    // Accounting is not admission. Only a future exact Owner phase may call this.
    [[nodiscard]] Status enter_writer_phase(Session &session, const void *owner)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (control_.phase != Phase::handoff || !handoff_attempted_ || !owner ||
            owner != owner_ || owner_ != owner_seal_ || control_.writer || !geometry_.writable)
            return reject(Status::denied("reclaim route private writer phase unavailable"));
        control_.writer = true;
        sync();
        return {};
    }
    [[nodiscard]] Status begin_write(Session &session, u64 first_sector, usize transfer_bytes = 1024)
    { return begin(session, Operation::write, first_sector, transfer_bytes); }
    [[nodiscard]] Status finish_write(Session &session, Status transport)
    { return finish(session, Operation::write, transport); }
    [[nodiscard]] Status begin_flush(Session &session)
    { return begin(session, Operation::flush, 0, 0); }
    [[nodiscard]] Status finish_flush(Session &session, Status transport)
    { return finish(session, Operation::flush, transport); }

    [[nodiscard]] Status begin(Session &session, Operation operation, u64 first_sector, usize transfer_bytes)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (operation != Operation::read && !control_.writer)
            return Status::denied("reclaim route WRITE/FLUSH require private admitted phase");
        if (operation == Operation::read && progress_.pending_writes)
            return Status::busy("reclaim route READ before successful FLUSH");
        if (operation != Operation::flush)
        {
            const auto transfer = session.transfer_limit();
            if (!transfer) return reject(transfer.status());
            if (first_sector % 2 || transfer_bytes != 1024 ||
                transfer_bytes % geometry_.block_size || transfer_bytes > transfer.value() ||
                first_sector >= geometry_.block_count ||
                transfer_bytes / geometry_.block_size > geometry_.block_count - first_sector)
                return Status::invalid_argument("reclaim route selected transfer bounds");
        }
        const usize index = usize(operation);
        if (index >= 3) return reject(Status::fault("reclaim route selected operation"));
        const auto bound = limits();
        const std::array<block::SessionCounter, 3> caps{bound.reads, bound.writes, bound.flushes};
        const auto attempts = attempted_counts(progress_), completions = completed_counts(progress_);
        if (control_.attempts[index] >= caps[index] || control_.completions[index] >= caps[index] ||
            attempts[index] == ~block::SessionCounter{0} || completions[index] == ~block::SessionCounter{0})
            return Status::overflow("reclaim route selected dispatch credit exhausted");
        flight_ = flight_seal_ = progress_;
        control_.operation = operation;
        control_.first_sector = first_sector;
        control_.transfer_bytes = transfer_bytes;
        sync();
        flight_control_ = control_;
        return {};
    }
    static void advance_attempt(block::SessionReport &report, Operation operation)
    {
        if (operation == Operation::read) ++report.reads;
        if (operation == Operation::write) ++report.writes;
        if (operation == Operation::flush) ++report.flushes;
    }
    static void advance_completion(block::SessionReport &report, Operation operation)
    {
        if (operation == Operation::read) ++report.completed_reads;
        if (operation == Operation::write) ++report.completed_writes;
        if (operation == Operation::flush) ++report.completed_flushes;
    }
    [[nodiscard]] Status finish(Session &session, Operation operation, Status transport)
    {
        if (anchor_.exact(&session) && original_ == &session)
        {
            const auto actual = session.report();
            if (actual) observed_ = actual.value();
        }
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != operation || operation == Operation::none)
            return reject(Status::fault("reclaim route post-dispatch retained authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        const bool exact_volume = same_geometry(session.geometry(), geometry_) && same_bounds(session.bounds(), bounds_);
        if (!exact_volume && transport)
            return reject(Status::fault("reclaim route post-dispatch original volume changed"));
        auto expected = flight_;
        advance_attempt(expected, operation);
        if (operation == Operation::write) expected.pending_writes = true;
        if (transport)
        {
            advance_completion(expected, operation);
            if (operation == Operation::flush) expected.pending_writes = false;
        }
        else
        {
            expected.state = block::DeviceState::quarantined;
            // An in-flight abort may retain an earlier error than transport.
            expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok)
                return reject(Status::fault("reclaim route transport failure lacks quarantine error"));
        }
        if (!reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("reclaim route transport attempt/completion prefix differs"));
        const usize index = usize(operation);
        ++control_.attempts[index];
        if (transport) ++control_.completions[index];
        progress_ = progress_seal_ = actual.value();
        control_.operation = Operation::none;
        control_.first_sector = 0;
        control_.transfer_bytes = 0;
        sync();
        if (!exact_volume)
            return reject(Status::fault("reclaim route post-dispatch original volume changed"));
        return transport ? Status{} : reject(transport);
    }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Control control_{}, sealed_{}, flight_control_{};
    const void *owner_{}, *owner_seal_{};
    bool attempted_{}, refine_attempted_{}, handoff_attempted_{};
};

template <usize> class Ext4ReclaimComposedHistory;
// Caller-owned, exclusive storage; never put this object on a kernel stack.
struct Ext4OrphanChainCaptureStorage
{
    struct BootstrapRead { u32 physical{}; Ext4OrphanBlock bytes{}; };
    std::array<BootstrapRead, 12> bootstrap_reads{};
    Ext4OrphanBlock superblock{}, descriptors{}, journal{};
    std::array<Ext4OrphanBlock, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<std::array<Ext4OrphanBlock, 512>, 4> inode_tables{};
    std::array<Ext4OrphanInodeImage, Ext4OrphanChainProof::capacity> chain{};
    std::array<Ext4OrphanDirectoryBlockImage,
               Ext4OrphanDirectoryReferenceScratch::block_capacity> directories{};
};

using Ext4OrphanSnapshotStorage = Ext4OrphanChainCaptureStorage;

enum class Ext4OrphanSnapshotPhase : u8 { unused, opening, admitted, checking, rejected, closed };
struct Ext4OrphanSnapshotReport
{
    Ext4OrphanSnapshotPhase phase{Ext4OrphanSnapshotPhase::unused};
    Status status{};
    u64 identity{}, registration_generation{}, session_serial{};
    u32 head{}, generation{}, sequence{};
    usize table_blocks{}, directory_blocks{}, chain_members{}, verified_blocks{};
    block::SessionReport original_io{};
    block::SessionCounter session_reads{};
    bool valid{}, prospective{};
};

// Private prospective seal, retained before the final physical comparison.
struct Ext4NamespaceSnapshotSeal
{
    Ext4OrphanChainCaptureStorage storage{};
    Ext4RecoveryInfo binding{};
    Ext4OrphanBlock normalized{};
    Ext4OrphanSnapshotReport report{};
    std::array<bool, 4> initialized{};
    const void *capture{}, *session{}, *source_storage{}, *scratch{}, *history{};
    usize chain_count{}, directory_count{}, bootstrap_count{};
    bool ready{}, terminal{};
};

[[nodiscard]] inline bool ext4_same_snapshot_storage(const Ext4OrphanChainCaptureStorage &a,
                                                    const Ext4OrphanChainCaptureStorage &b)
{
    if (a.superblock != b.superblock || a.descriptors != b.descriptors || a.journal != b.journal ||
        a.inode_bitmaps != b.inode_bitmaps || a.block_bitmaps != b.block_bitmaps || a.inode_tables != b.inode_tables)
        return false;
    for (usize index = 0; index < a.bootstrap_reads.size(); ++index)
        if (a.bootstrap_reads[index].physical != b.bootstrap_reads[index].physical ||
            a.bootstrap_reads[index].bytes != b.bootstrap_reads[index].bytes) return false;
    for (usize index = 0; index < a.chain.size(); ++index)
        if (a.chain[index].number != b.chain[index].number || a.chain[index].expected_generation != b.chain[index].expected_generation ||
            a.chain[index].table_block != b.chain[index].table_block || a.chain[index].table != b.chain[index].table) return false;
    for (usize index = 0; index < a.directories.size(); ++index)
        if (a.directories[index].inode != b.directories[index].inode || a.directories[index].logical != b.directories[index].logical ||
            a.directories[index].physical != b.directories[index].physical || a.directories[index].bytes != b.directories[index].bytes) return false;
    return true;
}

// Read-only, one-shot capture through the existing serialized owner session.
// Raw aliases, caches, other session users and caller-storage mutation must be
// excluded by that owner. This object cannot acquire/close a session or free an
// inode. Any read/integrity uncertainty aborts the borrowed session; an owning
// cache must also propagate the failure into its own quarantine state.
template <usize DeviceCapacity = 4>
class Ext4OrphanChainCapture final
{
    template <usize> friend class Ext4OrphanSnapshotAdmission;
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
    template <usize> friend class Ext4ReclaimResultHistory;
    template <usize> friend class Ext4ReclaimComposedHistory;
    using Registry = block::BlockSessions<DeviceCapacity>;
    using Block = Ext4OrphanBlock;
    using Session = typename Registry::Session;

    class UnguardedRead final
    {
      public:
        Status begin_read(Session &, u64, usize) { return {}; }
        Status finish_read(Session &, Status status) { return status; }
    };

    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    class BootstrapOverlay final : public driver::BlockDevice
    {
      public:
        explicit BootstrapOverlay(Ext4OrphanChainCapture &owner, Guard &guard) : owner_(owner), guard_(guard) {}
        driver::BlockGeometry geometry() const override { return owner_.session_->geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            if (auto status = owner_.observed_read(first, out, guard_); !status) return status;
            if (first % 2 || out.size() != 1024 || first / 2 >= 32768 ||
                owner_.bootstrap_count_ == owner_.storage_->bootstrap_reads.size())
                return Status::unsupported("orphan bootstrap observation budget");
            auto &observed = owner_.storage_->bootstrap_reads[owner_.bootstrap_count_++];
            observed.physical = u32(first / 2);
            for (usize at = 0; at < 1024; ++at) observed.bytes[at] = u8(out[at]);
            for (u64 sector = 2; sector < 4; ++sector)
                if (first <= sector && sector - first < out.size() / 512)
                    for (usize at = 0; at < 512; ++at)
                        out[(sector - first) * 512 + at] =
                            std::byte(owner_.normalized_[(sector - 2) * 512 + at]);
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("orphan bootstrap overlay is readonly"); }
        Status flush() override { return Status::denied("orphan bootstrap overlay is readonly"); }
      private:
        Ext4OrphanChainCapture &owner_;
        Guard &guard_;
    };

    // Repeat strict bootstrap checks on the retained image set. An earlier
    // checked transport read must not stand in for later inode-table bytes.
    class RetainedOverlay final : public driver::BlockDevice
    {
      public:
        explicit RetainedOverlay(const Ext4OrphanChainCapture &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.session_->geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            if (first % 2 || out.size() != 1024 || first / 2 >= 32768)
                return Status::invalid_argument("orphan retained bootstrap read bounds");
            const u32 physical = u32(first / 2);
            const Block *image{};
            if (physical == 1) image = &owner_.normalized_;
            else if (physical == 2) image = &owner_.storage_->descriptors;
            else if (physical == owner_.binding_.layout.journal_first_block) image = &owner_.storage_->journal;
            else for (usize group = 0; group < 4; ++group)
            {
                const auto &metadata = owner_.binding_.metadata;
                if (physical == metadata[group * 4 + 1].first) image = &owner_.storage_->block_bitmaps[group];
                if (physical == metadata[group * 4 + 2].first) image = &owner_.storage_->inode_bitmaps[group];
                const auto table = metadata[group * 4 + 3];
                if (owner_.initialized_[group] && physical >= table.first && physical - table.first < 512)
                    image = &owner_.storage_->inode_tables[group][physical - table.first];
            }
            if (!image) return Status::io_error("orphan retained bootstrap image absent");
            for (usize at = 0; at < 1024; ++at) out[at] = std::byte((*image)[at]);
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("orphan retained bootstrap is readonly"); }
        Status flush() override { return Status::denied("orphan retained bootstrap is readonly"); }
      private:
        const Ext4OrphanChainCapture &owner_;
    };

  public:
    Ext4OrphanChainCapture() = default;
    Ext4OrphanChainCapture(const Ext4OrphanChainCapture &) = delete;
    Ext4OrphanChainCapture &operator=(const Ext4OrphanChainCapture &) = delete;
    Ext4OrphanChainCapture(Ext4OrphanChainCapture &&) = delete;
    Ext4OrphanChainCapture &operator=(Ext4OrphanChainCapture &&) = delete;

    [[nodiscard]] Status capture(Session &session, Ext4OrphanChainCaptureStorage &storage,
                                 Ext4OrphanDirectoryQualifiedReclaimScratch &scratch)
    {
        UnguardedRead guard;
        return capture(session, storage, scratch, guard);
    }

    // Guard is borrowed only for this call, including physical bootstrap reads.
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status capture(Session &session, Ext4OrphanChainCaptureStorage &storage,
                                 Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, Guard &guard)
    {
        if (report_.phase != Ext4OrphanSnapshotPhase::unused)
            return Status::already_exists("orphan snapshot is one-shot");
        report_.phase = Ext4OrphanSnapshotPhase::opening;
        session_ = &session;
        if (!reclaim_anchor_bound_)
        {
            if (auto status = reclaim_session_anchor_.bind(session); !status) return reject(status);
            reclaim_anchor_bound_ = true;
        }
        storage_ = &storage; scratch_ = &scratch;
        const auto initial = session_->report();
        if (!initial) return reject(initial.status());
        report_.identity = initial.value().identity;
        report_.registration_generation = initial.value().registration_generation;
        report_.session_serial = initial.value().session_serial;
        report_.original_io = initial.value();
        if (auto status = stable(); !status) return reject(status);
        if (session_->geometry().block_size != 512 || session_->geometry().block_count < 65536)
            return reject(Status::unsupported("orphan snapshot partition geometry"));
        const auto limit = session_->transfer_limit();
        if (!limit) return reject(limit.status());
        if (limit.value() < 1024)
            return reject(Status::unsupported("orphan snapshot transfer bound"));
        if (auto status = read(1, storage_->superblock, guard); !status) return reject(status);
        const auto bytes = std::span<const u8>{storage_->superblock};
        if (jbd2::detail::crc32c(~u32{0}, bytes.first(1020)) != ext4_checked::le32(bytes, 1020))
            return reject(Status::io_error("orphan snapshot superblock checksum"));
        const u32 head = ext4_checked::le32(bytes, 232);
        if (head < 11 || head > 8192 || ext4_checked::le32(bytes, 96) != 0x46 ||
            ext4_checked::le32(bytes, 0) != 8192 || ext4_checked::le32(bytes, 4) != 32768 ||
            ext4_checked::le32(bytes, 24) || ext4_checked::le32(bytes, 32) != 8192 ||
            ext4_checked::le32(bytes, 40) != 2048)
            return reject(Status::unsupported("orphan snapshot cold recovery profile"));
        normalized_ = storage_->superblock;
        store32(normalized_, 232, 0);
        store32(normalized_, 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{normalized_}.first(1020)));
        BootstrapOverlay<Guard> overlay(*this, guard);
        if (auto status = bootstrap_.inspect(overlay, {0, session_->geometry().block_count}); !status)
            return reject(status);
        const auto *info = bootstrap_.info();
        if (!info || !info->needs_recovery || !info->empty_log || info->metadata_count != 16 ||
            info->layout.block_size != 1024 || info->layout.filesystem_blocks != 32768)
            return reject(Status::unsupported("orphan snapshot requires empty bounded journal"));
        binding_ = *info;
        if (auto status = read(2, storage_->descriptors, guard); !status) return reject(status);
        if (auto status = read(u32(binding_.layout.journal_first_block), storage_->journal, guard); !status)
            return reject(status);
        const auto codec = jbd2::Codec::open(storage_->journal, binding_.layout.journal_blocks,
            binding_.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return reject(codec.status());
        if (codec.value().superblock().start_block ||
            codec.value().superblock().sequence != binding_.journal_sequence)
            return reject(Status::io_error("orphan snapshot journal changed during bootstrap"));
        for (u32 group = 0; group < 4; ++group)
        {
            const usize at = group * 32;
            if (ext4_checked::le32(storage_->descriptors, at) != binding_.metadata[group * 4 + 1].first ||
                ext4_checked::le32(storage_->descriptors, at + 4) != binding_.metadata[group * 4 + 2].first ||
                ext4_checked::le32(storage_->descriptors, at + 8) != binding_.metadata[group * 4 + 3].first)
                return reject(Status::io_error("orphan snapshot descriptor placement changed"));
            if (auto status = read(u32(binding_.metadata[group * 4 + 1].first),
                                   storage_->block_bitmaps[group], guard); !status) return reject(status);
            if (auto status = read(u32(binding_.metadata[group * 4 + 2].first),
                                   storage_->inode_bitmaps[group], guard); !status) return reject(status);
            initialized_[group] = !(ext4_checked::le16(storage_->descriptors, at + 18) & 1);
            if (initialized_[group])
                for (u32 offset = 0; offset < 512; ++offset)
                {
                    if (auto status = read(u32(binding_.metadata[group * 4 + 3].first) + offset,
                                           storage_->inode_tables[group][offset], guard); !status)
                        return reject(status);
                    ++report_.table_blocks;
                }
        }
        RetainedOverlay retained(*this);
        if (auto status = retained_bootstrap_.inspect(retained, {0, session_->geometry().block_count}); !status)
            return reject(status);
        const auto *checked = retained_bootstrap_.info();
        if (!checked || checked->uuid != binding_.uuid || checked->metadata_count != binding_.metadata_count ||
            checked->journal_inode != binding_.journal_inode || checked->journal_sequence != binding_.journal_sequence ||
            checked->needs_recovery != binding_.needs_recovery || checked->empty_log != binding_.empty_log ||
            checked->layout.first_sector != binding_.layout.first_sector ||
            checked->layout.block_size != binding_.layout.block_size ||
            checked->layout.filesystem_blocks != binding_.layout.filesystem_blocks ||
            checked->layout.journal_first_block != binding_.layout.journal_first_block ||
            checked->layout.journal_blocks != binding_.layout.journal_blocks)
            return reject(Status::io_error("orphan retained bootstrap binding differs"));
        for (usize index = 0; index < binding_.metadata_count; ++index)
            if (checked->metadata[index].first != binding_.metadata[index].first ||
                checked->metadata[index].count != binding_.metadata[index].count)
                return reject(Status::io_error("orphan retained bootstrap metadata differs"));
        if (auto status = collect_chain(head); !status) return reject(status);
        Ext4OrphanOwnershipProof ownership{};
        if (auto status = Ext4OrphanOwnershipValidator::validate(
                binding_, source().ownership, scratch_->reclaim, ownership); !status) return reject(status);
        if (auto status = collect_directories(guard); !status) return reject(status);
        Ext4OrphanDirectoryReferenceProof directory{};
        if (auto status = Ext4OrphanDirectoryReferenceValidator::validate(
                binding_, source(), scratch_->directory, directory); !status) return reject(status);
        report_.head = head;
        report_.generation = storage_->chain[0].expected_generation;
        report_.sequence = binding_.journal_sequence;
        report_.chain_members = chain_count_; report_.directory_blocks = directory_count_;
        if (namespace_seal_)
        {
            if (namespace_seal_->ready) return reject(Status::already_exists("namespace snapshot seal is one-shot"));
            namespace_seal_->storage = *storage_;
            namespace_seal_->binding = binding_; namespace_seal_->normalized = normalized_;
            namespace_seal_->report = report_; namespace_seal_->initialized = initialized_;
            namespace_seal_->chain_count = chain_count_; namespace_seal_->directory_count = directory_count_;
            namespace_seal_->bootstrap_count = bootstrap_count_; namespace_seal_->ready = true;
            namespace_seal_->capture = this; namespace_seal_->session = session_;
            namespace_seal_->source_storage = storage_; namespace_seal_->scratch = scratch_;
            namespace_seal_->history = namespace_history_ ? static_cast<const void *>(namespace_history_) : reclaim_history_ ? static_cast<const void *>(reclaim_history_) : composed_history_;
            namespace_sealed_ = true;
        }
        if (auto status = compare_all(guard); !status) return reject(status);
        if (namespace_seal_ && report_.verified_blocks !=
                11 + namespace_seal_->report.table_blocks + namespace_seal_->directory_count + namespace_seal_->bootstrap_count)
            return reject(Status::fault("namespace prospective terminal verified block count differs"));
        report_.phase = Ext4OrphanSnapshotPhase::admitted; report_.valid = true;
        if (namespace_seal_) { namespace_seal_->report = report_; namespace_seal_->terminal = true; }
        return {};
    }

    [[nodiscard]] Status plan_head(u32 head, u32 generation,
                                   Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        UnguardedRead guard;
        return plan_head(head, generation, output, guard);
    }

    // A later comparison borrows its own Guard; capture retains no read hook.
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status plan_head(u32 head, u32 generation,
                                   Ext4OrphanDirectoryQualifiedReclaimPlan &output, Guard &guard)
    {
        output = {};
        if (report_.prospective) return Status::denied("prospective namespace bytes cannot authorize reclaim planning");
        if (report_.phase != Ext4OrphanSnapshotPhase::admitted || !report_.valid)
            return Status::not_initialized("orphan snapshot is not admitted");
        if (head != report_.head || generation != report_.generation)
            return Status::io_error("orphan snapshot stale requested identity");
        report_.phase = Ext4OrphanSnapshotPhase::checking;
        if (auto status = compare_all(guard); !status) return reject(status);
        if (auto status = Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(
                binding_, source(), *scratch_, head, generation, output); !status) return reject(status);
        report_.phase = Ext4OrphanSnapshotPhase::admitted;
        return {};
    }

    [[nodiscard]] const Ext4OrphanSnapshotReport &report() const { return report_; }

  private:
    [[nodiscard]] Status capture_namespace_result(Session &session, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, const Ext4NamespaceResultHistory &history,
        Ext4NamespaceSnapshotSeal &seal)
    {
        if (report_.phase != Ext4OrphanSnapshotPhase::unused || namespace_history_)
            return Status::already_exists("prospective namespace capture is one-shot");
        if (seal.ready) return Status::already_exists("namespace snapshot seal is already retained");
        namespace_history_ = &history; namespace_seal_ = &seal; report_.prospective = true;
        return capture(session, storage, scratch);
    }
    [[nodiscard]] Status capture_reclaim_result(Session &session, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, Ext4ReclaimResultHistory<DeviceCapacity> &history,
        Ext4NamespaceSnapshotSeal &seal)
    {
        UnguardedRead guard;
        return capture_reclaim_result(session, storage, scratch, history, seal, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status capture_reclaim_result(Session &session, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, Ext4ReclaimResultHistory<DeviceCapacity> &history,
        Ext4NamespaceSnapshotSeal &seal, Guard &guard)
    {
        if (report_.phase != Ext4OrphanSnapshotPhase::unused || namespace_history_ || reclaim_history_ || seal.ready)
            return Status::already_exists("prospective active reclaim capture is one-shot");
        if (history.original_session_ != &session || !history.member_count_)
            return Status::denied("prospective active reclaim capture requires original Session and nonzero head");
        if (auto status = reclaim_session_anchor_.bind(session); !status) return status;
        reclaim_anchor_bound_ = true;
        reclaim_history_ = reclaim_origin_ = &history; namespace_seal_ = &seal; report_.prospective = true;
        return capture(session, storage, scratch, guard);
    }
    [[nodiscard]] Status capture_composed_result(Session &session, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, Ext4ReclaimComposedHistory<DeviceCapacity> &history,
        Ext4NamespaceSnapshotSeal &seal)
    {
        UnguardedRead guard;
        return capture_composed_result(session, storage, scratch, history, seal, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status capture_composed_result(Session &session, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, Ext4ReclaimComposedHistory<DeviceCapacity> &history,
        Ext4NamespaceSnapshotSeal &seal, Guard &guard)
    {
        if (report_.phase != Ext4OrphanSnapshotPhase::unused || namespace_history_ || reclaim_history_ || composed_history_ || seal.ready)
            return Status::already_exists("composed dry capture is one-shot");
        if (history.original_session_ != &session || history.index_ >= history.member_count_)
            return Status::denied("composed dry capture requires original Session and remaining member");
        if (auto status = reclaim_session_anchor_.bind(session); !status) return status;
        reclaim_anchor_bound_ = true; composed_history_ = composed_origin_ = &history;
        namespace_seal_ = &seal; report_.prospective = true;
        return capture(session, storage, scratch, guard);
    }
    // The output stays inside the composed route; public prospective plan_head remains denied.
    [[nodiscard]] Status dry_plan(Ext4ReclaimComposedHistory<DeviceCapacity> &route,
        Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        UnguardedRead guard;
        return dry_plan(route, output, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status dry_plan(Ext4ReclaimComposedHistory<DeviceCapacity> &route,
        Ext4OrphanDirectoryQualifiedReclaimPlan &output, Guard &guard)
    {
        if (composed_history_ != &route || report_.phase != Ext4OrphanSnapshotPhase::admitted || !report_.prospective)
            return Status::denied("composed private dry plan requires current prospective capture");
        if (auto status = namespace_storage_exact(); !status) return reject(status);
        if (auto status = route.compare_capture(*this, guard); !status) return reject(status);
        if (auto status = Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(binding_, source(), *scratch_,
            report_.head, report_.generation, output); !status) return reject(status);
        return namespace_storage_exact();
    }
    [[nodiscard]] Status namespace_storage_exact() const
    {
        if (!namespace_sealed_)
        {
            if (namespace_seal_ && (namespace_seal_->ready || namespace_seal_->terminal || report_.verified_blocks || report_.valid))
                return Status::fault("namespace prospective retained result seal establishment changed");
            return {};
        }
        if (!namespace_seal_ || !namespace_seal_->ready)
            return Status::fault("namespace prospective retained result seal lost");
        const auto &seal = *namespace_seal_; const auto &old = seal.report;
        if ((!namespace_history_ && !reclaim_history_ && !composed_history_) ||
            (usize(bool(namespace_history_)) + usize(bool(reclaim_history_)) + usize(bool(composed_history_)) != 1) || !report_.prospective || !storage_ || seal.capture != this ||
            seal.session != session_ || seal.source_storage != storage_ || seal.scratch != scratch_ || seal.history != (namespace_history_ ? static_cast<const void *>(namespace_history_) : reclaim_history_ ? static_cast<const void *>(reclaim_history_) : composed_history_) ||
            !ext4_same_snapshot_storage(*storage_, seal.storage) || normalized_ != seal.normalized ||
            !reclaim_history::same_info(binding_, seal.binding) || initialized_ != seal.initialized ||
            chain_count_ != seal.chain_count || directory_count_ != seal.directory_count || bootstrap_count_ != seal.bootstrap_count ||
            report_.identity != old.identity || report_.registration_generation != old.registration_generation ||
            report_.session_serial != old.session_serial || report_.head != old.head || report_.generation != old.generation ||
            report_.sequence != old.sequence || report_.table_blocks != old.table_blocks || report_.directory_blocks != old.directory_blocks ||
            report_.chain_members != old.chain_members || report_.status.code() != StatusCode::ok ||
            report_.valid != seal.terminal || report_.phase != (seal.terminal ? Ext4OrphanSnapshotPhase::admitted : Ext4OrphanSnapshotPhase::opening))
            return Status::fault("namespace prospective retained result storage or structure changed");
        if (seal.terminal && (report_.session_reads != old.session_reads || report_.verified_blocks != old.verified_blocks))
            return Status::fault("namespace prospective terminal capture counters changed");
        const auto &a = report_.original_io, &b = old.original_io;
        if (a.state != b.state || a.identity != b.identity || a.registration_generation != b.registration_generation ||
            a.session_serial != b.session_serial || a.reads != b.reads || a.completed_reads != b.completed_reads ||
            a.writes != b.writes || a.completed_writes != b.completed_writes || a.flushes != b.flushes ||
            a.completed_flushes != b.completed_flushes || a.in_flight != b.in_flight || a.pending_writes != b.pending_writes ||
            a.first_error != b.first_error)
            return Status::fault("namespace prospective original result counters changed");
        return {};
    }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    [[nodiscard]] Status reject(Status status)
    {
        report_.valid = false; report_.status = status;
        report_.phase = Ext4OrphanSnapshotPhase::rejected;
        // Every entry point binds independent original anchors before reads.
        // A callback-mutated attachment or flag cannot redirect quarantine.
        static_cast<void>(reclaim_session_anchor_.abort(status));
        return status;
    }
    [[nodiscard]] Status stable() const
    {
        if (composed_history_ != composed_origin_ || reclaim_history_ != reclaim_origin_ || (!reclaim_anchor_bound_ || !reclaim_session_anchor_.exact(session_)))
            return Status::fault("active reclaim original nested history or Session identity changed");
        if (auto status = namespace_storage_exact(); !status) return status;
        if (namespace_history_)
            if (auto status = namespace_history_->stable(); !status) return status;
        if (reclaim_history_)
            if (auto status = reclaim_history_->stable(); !status) return status;
        if (composed_history_)
            if (auto status = composed_history_->stable(); !status) return status;
        const auto current = session_->report();
        if (!current) return current.status();
        const auto &value = current.value();
        if (value.state != block::DeviceState::owned || value.in_flight || value.pending_writes ||
            value.identity != report_.identity || value.registration_generation != report_.registration_generation ||
            value.session_serial != report_.session_serial || value.first_error != StatusCode::ok ||
            value.writes != report_.original_io.writes || value.flushes != report_.original_io.flushes ||
            value.completed_writes != value.writes || value.completed_flushes != value.flushes ||
            value.completed_writes != report_.original_io.completed_writes ||
            value.completed_flushes != report_.original_io.completed_flushes ||
            value.reads != report_.original_io.reads + report_.session_reads ||
            value.completed_reads != value.reads)
            return Status::io_error("orphan snapshot session changed");
        return {};
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status read(u32 physical, Block &output, Guard &guard)
    {
        if (physical >= 32768) return Status::io_error("orphan snapshot physical block bounds");
        return observed_read(u64{physical} * 2, std::as_writable_bytes(std::span<u8>{output}), guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status observed_read(u64 first, std::span<std::byte> output, Guard &guard)
    {
        if (first % 2 || first >= 65536 || output.size() != 1024 ||
            report_.session_reads == ~block::SessionCounter{0} ||
            report_.original_io.reads > ~block::SessionCounter{0} - report_.session_reads - 1)
            return Status::overflow("orphan capture read bounds or counter");
        if (auto status = stable(); !status) return status;
        const auto paused = report_;
        auto *source = storage_; auto *scratch = scratch_; auto *session = session_;
        auto *history = reclaim_history_; auto *composed = composed_history_; auto *seal = namespace_seal_;
        const auto *namespace_history = namespace_history_;
        const auto result = composed ? composed->read_blocks(first, output, guard) :
            history ? history->read_blocks(first, output, guard) : block::guarded_read_blocks(*session, guard, first, output);
        if (!result) return result;
        if (history && (storage_ != source || scratch_ != scratch || session_ != session || reclaim_history_ != history ||
            namespace_seal_ != seal || !same_report(report_, paused)))
            return history->reject(Status::fault("active reclaim nested callback changed paused capture"));
        if (composed && (storage_ != source || scratch_ != scratch || session_ != session || composed_history_ != composed ||
            namespace_seal_ != seal || !same_report(report_, paused)))
            return composed->reject(Status::fault("composed nested callback changed paused capture"));
        if (!history && !composed && (storage_ != source || scratch_ != scratch || session_ != session ||
            reclaim_history_ || composed_history_ || namespace_history_ != namespace_history || namespace_seal_ != seal ||
            !reclaim_anchor_bound_ || !reclaim_session_anchor_.exact(session) || !same_report(report_, paused)))
            return Status::fault("orphan capture callback changed paused original read");
        ++report_.session_reads;
        if (namespace_history_)
            if (auto status = namespace_history_->substitute(u32(first / 2), output); !status) return status;
        return stable();
    }
    [[nodiscard]] Ext4OrphanDirectoryReferenceSource source() const
    {
        std::array<std::span<const Block>, 4> tables{};
        for (usize group = 0; group < 4; ++group)
            if (initialized_[group]) tables[group] = storage_->inode_tables[group];
        return {{{storage_->superblock, storage_->descriptors, storage_->inode_bitmaps,
                  storage_->block_bitmaps, std::span<const Ext4OrphanInodeImage>{storage_->chain}.first(chain_count_)},
                 tables}, std::span<const Ext4OrphanDirectoryBlockImage>{storage_->directories}.first(directory_count_)};
    }
    [[nodiscard]] Status collect_chain(u32 number)
    {
        while (number)
        {
            if (number < 11 || number > 8192 || chain_count_ == storage_->chain.size())
                return Status::unsupported("orphan snapshot chain bounds");
            for (usize prior = 0; prior < chain_count_; ++prior)
                if (storage_->chain[prior].number == number)
                    return Status::io_error("orphan snapshot chain cycle");
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (!initialized_[group]) return Status::io_error("orphan snapshot uninitialized chain table");
            auto &entry = storage_->chain[chain_count_++];
            entry.number = number;
            entry.table_block = u32(binding_.metadata[group * 4 + 3].first) + local / 4;
            entry.table = storage_->inode_tables[group][local / 4];
            const auto inode = std::span<const u8>{entry.table}.subspan((local % 4) * 256, 256);
            entry.expected_generation = ext4_checked::le32(inode, 100);
            number = ext4_checked::le32(inode, 20);
        }
        return {};
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status collect_directories(Guard &guard)
    {
        for (u32 number = 1; number <= 8192; ++number)
        {
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (!initialized_[group] ||
                !(storage_->inode_bitmaps[group][local / 8] & (1U << (local % 8)))) continue;
            const auto inode = std::span<const u8>{storage_->inode_tables[group][local / 4]}
                .subspan((local % 4) * 256, 256);
            if ((ext4_checked::le16(inode, 0) & 0170000) != 0040000) continue;
            const u32 size = ext4_checked::le32(inode, 4);
            if (!size || size > 65536 || size % 1024)
                return Status::unsupported("orphan snapshot directory size");
            for (u32 logical = 0; logical < size / 1024; ++logical)
            {
                if (directory_count_ == storage_->directories.size())
                    return Status::no_memory("orphan snapshot directory block budget");
                u32 physical = 0;
                for (u32 record = 0; record < ext4_checked::le16(inode, 42); ++record)
                {
                    const usize at = 52 + record * 12;
                    const u32 first = ext4_checked::le32(inode, at), length = ext4_checked::le16(inode, at + 4);
                    if (logical >= first && logical - first < length)
                        physical = ext4_checked::le32(inode, at + 8) + logical - first;
                }
                if (!physical) return Status::io_error("orphan snapshot directory mapping absent");
                auto &entry = storage_->directories[directory_count_++];
                entry.inode = number; entry.logical = logical; entry.physical = physical;
                if (auto status = read(physical, entry.bytes, guard); !status) return status;
            }
        }
        return {};
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare(u32 physical, const Block &expected, Guard &guard)
    {
        if (auto status = read(physical, checking_, guard); !status) return status;
        if (checking_ != expected) return Status::io_error("orphan snapshot whole block changed");
        ++report_.verified_blocks;
        return {};
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_all(Guard &guard)
    {
        if (auto status = compare(1, storage_->superblock, guard); !status) return status;
        if (auto status = compare(2, storage_->descriptors, guard); !status) return status;
        if (auto status = compare(u32(binding_.layout.journal_first_block), storage_->journal, guard); !status) return status;
        for (u32 group = 0; group < 4; ++group)
        {
            if (auto status = compare(u32(binding_.metadata[group * 4 + 1].first),
                                       storage_->block_bitmaps[group], guard); !status) return status;
            if (auto status = compare(u32(binding_.metadata[group * 4 + 2].first),
                                       storage_->inode_bitmaps[group], guard); !status) return status;
            if (initialized_[group])
                for (u32 offset = 0; offset < 512; ++offset)
                    if (auto status = compare(u32(binding_.metadata[group * 4 + 3].first) + offset,
                                               storage_->inode_tables[group][offset], guard); !status) return status;
        }
        for (usize index = 0; index < directory_count_; ++index)
            if (auto status = compare(storage_->directories[index].physical,
                                       storage_->directories[index].bytes, guard); !status) return status;
        for (usize index = 0; index < bootstrap_count_; ++index)
            if (auto status = compare(storage_->bootstrap_reads[index].physical,
                                       storage_->bootstrap_reads[index].bytes, guard); !status) return status;
        return stable();
    }

    static bool same_report(const Ext4OrphanSnapshotReport &a, const Ext4OrphanSnapshotReport &b)
    {
        return a.phase == b.phase && reclaim_history::same_status(a.status, b.status) &&
            a.identity == b.identity && a.registration_generation == b.registration_generation && a.session_serial == b.session_serial &&
            a.head == b.head && a.generation == b.generation && a.sequence == b.sequence && a.table_blocks == b.table_blocks &&
            a.directory_blocks == b.directory_blocks && a.chain_members == b.chain_members && a.verified_blocks == b.verified_blocks &&
            reclaim_history::same_session(a.original_io, b.original_io) && a.session_reads == b.session_reads &&
            a.valid == b.valid && a.prospective == b.prospective;
    }
    reclaim_history::SessionAnchor<Session> reclaim_session_anchor_{};
    bool reclaim_anchor_bound_{};
    Session *session_{};
    Ext4ReclaimResultHistory<DeviceCapacity> *reclaim_history_{}, *reclaim_origin_{};
    Ext4ReclaimComposedHistory<DeviceCapacity> *composed_history_{}, *composed_origin_{};
    const Ext4NamespaceResultHistory *namespace_history_{};
    Ext4NamespaceSnapshotSeal *namespace_seal_{};
    bool namespace_sealed_{};
    Ext4RecoveryBinding bootstrap_{};
    Ext4RecoveryBinding retained_bootstrap_{};
    Ext4RecoveryInfo binding_{};
    Ext4OrphanSnapshotStorage *storage_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch *scratch_{};
    Block normalized_{}, checking_{};
    std::array<bool, 4> initialized_{};
    usize chain_count_{}, directory_count_{}, bootstrap_count_{};
    Ext4OrphanSnapshotReport report_{};
};

// Permanent caller-owned storage. Rows contain bounded images, never full table copies.
class Ext4ReclaimComposedStorage final
{
    template <usize> friend class Ext4ReclaimComposedHistory;
    template <usize, usize> friend class Ext4OwnedTransaction;
    struct DryRow
    {
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        Ext4OrphanReclaimJournalPlan wal{};
        Ext4OrphanBlock cleanup{};
        u32 inode{}, generation{}, next{}, seconds{}, nanoseconds{}, cleanup_sequence{};
        usize index{};
        bool ready{};
    };
  public:
    Ext4ReclaimComposedStorage() = default;
    Ext4ReclaimComposedStorage(const Ext4ReclaimComposedStorage &) = delete;
    Ext4ReclaimComposedStorage &operator=(const Ext4ReclaimComposedStorage &) = delete;
    Ext4ReclaimComposedStorage(Ext4ReclaimComposedStorage &&) = delete;
    Ext4ReclaimComposedStorage &operator=(Ext4ReclaimComposedStorage &&) = delete;
  private:
    const void *claimant_{};
    std::array<DryRow, 8> rows_{}, sealed_rows_{};
    DryRow pending_{};
    Ext4OrphanBlock extension_{}, sealed_extension_{}, checking_{};
    Ext4OrphanChainCaptureStorage capture_storage_{};
    Ext4NamespaceSnapshotSeal capture_seal_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch scratch_{};
    Ext4OrphanReclaimJournalScratch formatter_{};
};

// Private readonly dry route. No public validity bit or row grants writer authority.
template <usize DeviceCapacity = 4>
class Ext4ReclaimComposedHistory final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4OrphanChainCapture;
    using Origin = Ext4ReclaimResultHistory<DeviceCapacity>;
    using UnguardedRead = typename Origin::UnguardedRead;
    using Session = typename block::BlockSessions<DeviceCapacity>::Session;
    using Block = Ext4OrphanBlock;
    using Row = Ext4ReclaimComposedStorage::DryRow;
    using Capture = Ext4OrphanChainCapture<DeviceCapacity>;
    using Completion = Ext4RecoveryCompletion<6, 2>;
    struct Member { u32 inode{}, generation{}, next{}; };
    using ReadPhase = Ext4ReclaimDryPhase;
    struct Seal
    {
        Origin *origin{};
        Ext4ReclaimComposedStorage *storage{};
        Session *session{};
        std::array<Member, 7> members{};
        block::SessionReport initial{}, progress{};
        usize count{}, index{}, rows{};
        block::SessionCounter reads{};
        bool ready{}, reading{}, extension{}, complete{}, ordinary{}, endpoint{}, capture{};
        ReadPhase phase{ReadPhase::unused};
        std::array<block::SessionCounter, 4> charged{};
        block::SessionCounter phase_reads{}, extension_reads{};
    };
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    class OrdinaryView final : public driver::BlockDevice
    {
      public:
        explicit OrdinaryView(Ext4ReclaimComposedHistory &owner, Guard &guard) : owner_(owner), guard_(guard) {}
        driver::BlockGeometry geometry() const override { return owner_.original_session_->geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> bytes) override { return owner_.read_blocks(first, bytes, guard_); }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("composed ordinary dry endpoint cannot write"); }
        Status flush() override { return Status::denied("composed ordinary dry endpoint cannot flush"); }
      private:
        Ext4ReclaimComposedHistory &owner_;
        Guard &guard_;
    };
  public:
    Ext4ReclaimComposedHistory() = default;
    Ext4ReclaimComposedHistory(const Ext4ReclaimComposedHistory &) = delete;
    Ext4ReclaimComposedHistory &operator=(const Ext4ReclaimComposedHistory &) = delete;
    Ext4ReclaimComposedHistory(Ext4ReclaimComposedHistory &&) = delete;
    Ext4ReclaimComposedHistory &operator=(Ext4ReclaimComposedHistory &&) = delete;
  private:
    static void be32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - 8 * index)); }
    static bool same_row(const Row &a, const Row &b)
    {
        return reclaim_history::same_plan(a.plan.reclaim, b.plan.reclaim) &&
            reclaim_history::same_directory(a.plan.directory, b.plan.directory) && a.plan.valid == b.plan.valid &&
            reclaim_history::same_journal(a.wal, b.wal) && a.cleanup == b.cleanup &&
            a.inode == b.inode && a.generation == b.generation && a.next == b.next &&
            a.seconds == b.seconds && a.nanoseconds == b.nanoseconds && a.cleanup_sequence == b.cleanup_sequence &&
            a.index == b.index && a.ready == b.ready;
    }
    static bool same_completion(const Ext4CompletionReport &a, const Ext4CompletionReport &b)
    {
        return a.phase == b.phase && a.failed_phase == b.failed_phase && reclaim_history::same_status(a.status, b.status) &&
            a.validated_actions == b.validated_actions && a.checked_inodes == b.checked_inodes &&
            a.write_attempts == b.write_attempts && a.completed_writes == b.completed_writes &&
            a.flush_attempts == b.flush_attempts && a.completed_flushes == b.completed_flushes &&
            a.journal_sequence == b.journal_sequence && a.empty_log == b.empty_log && a.already_clean == b.already_clean &&
            a.home_durable == b.home_durable && a.journal_durable == b.journal_durable && a.filesystem_durable == b.filesystem_durable;
    }
    [[nodiscard]] Status reject(Status status)
    {
        rejected_ = true;
        static_cast<void>(session_anchor_.abort(status));
        return status;
    }
    [[nodiscard]] Status prepare(Origin &origin, Ext4ReclaimComposedStorage &storage)
    {
        UnguardedRead guard;
        return prepare(origin, storage, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status prepare(Origin &origin, Ext4ReclaimComposedStorage &storage, Guard &guard)
    {
        if (attempted_) return Status::already_exists("composed dry route is one-shot");
        attempted_ = true;
        if (storage.claimant_) return origin.reject(Status::busy("composed permanent storage already claimed"));
        storage.claimant_ = this;
        if (auto status = origin.stable(); !status) return origin.reject(status);
        origin_ = &origin; storage_ = &storage; original_session_ = origin.original_session_;
        if (auto status = session_anchor_.bind(*original_session_); !status) return status;
        member_count_ = origin.member_count_;
        for (usize at = 0; at < members_.size(); ++at)
            members_[at] = {origin.members_[at].inode, origin.members_[at].generation, origin.members_[at].next};
        initial_ = progress_ = origin.progress_;
        if (auto status = budget_.reserve(initial_, member_count_, origin.binding_->capture_.retained_.count == 4,
            origin.binding_->capture_.retained_.report.semantic.active_sequence); !status) return reject(status);
        auto &row = storage.rows_[0];
        row.plan = origin.binding_->capture_.retained_.plan;
        row.wal = origin.binding_->capture_.retained_.journal;
        row.cleanup = origin.empty_head_; row.cleanup_sequence = origin.binding_->capture_.retained_.report.semantic.cleanup_sequence;
        row.inode = row.plan.reclaim.inode; row.generation = row.plan.reclaim.old_generation; row.next = row.plan.reclaim.next_head;
        row.ready = true; storage.sealed_rows_[0] = row;
        row_count_ = 1;
        seal_ = {origin_, storage_, original_session_, members_, initial_, progress_, member_count_, 0, 1, 0, true};
        ready_ = true;
        if (auto status = stable(); !status) return reject(status);
        if (origin.binding_->capture_.retained_.count == 4)
        {
            if (extension_reads_ >= budget_.capacity_.extension)
                return reject(Status::overflow("composed named extension reservation exhausted before dispatch"));
            // Exactly one named observation, integrated with the original history's counters.
            reading_ = seal_.reading = true;
            const auto paused = progress_;
            const auto status = origin.extension_read(std::as_writable_bytes(std::span{storage.extension_}), nullptr, guard);
            if (!status) return reject(status);
            if (auto guarded = retained_exact(); !guarded) return reject(guarded);
            if (auto guarded = adopt(paused); !guarded) return reject(guarded);
            reading_ = seal_.reading = false;
            storage.sealed_extension_ = storage.extension_; extension_ = seal_.extension = true;
            extension_reads_ = seal_.extension_reads = 1;
        }
        return stable();
    }
    [[nodiscard]] Status retained_exact() const
    {
        if (!attempted_ || !ready_ || rejected_ || !seal_.ready || origin_ != seal_.origin || storage_ != seal_.storage ||
            !origin_ || !storage_ || storage_->claimant_ != this || !session_anchor_.exact(original_session_) || original_session_ != seal_.session ||
            original_session_ != origin_->original_session_ || member_count_ != seal_.count || member_count_ > 7 ||
            index_ != seal_.index || row_count_ != seal_.rows || row_count_ != index_ + 1 || row_count_ > 8 || index_ > member_count_ ||
            reads_ != seal_.reads || reading_ != seal_.reading || extension_ != seal_.extension || complete_ != seal_.complete ||
            ordinary_ != seal_.ordinary || endpoint_ != seal_.endpoint || capture_ready_ != seal_.capture ||
            phase_ != seal_.phase || charged_ != seal_.charged || phase_reads_ != seal_.phase_reads || extension_reads_ != seal_.extension_reads ||
            (endpoint_ && !same_completion(completion_.report(), terminal_report_)) || !reclaim_history::same_session(initial_, seal_.initial) ||
            !reclaim_history::same_session(progress_, seal_.progress))
            return Status::fault("composed original domain or exact progress changed");
        if (initial_.reads > ~block::SessionCounter{0} - reads_) return Status::overflow("composed cumulative reads overflow");
        auto expected = initial_; expected.reads += reads_; expected.completed_reads += reads_;
        if (!reclaim_history::same_session(expected, progress_)) return Status::fault("composed exact read equation changed");
        if (auto status = budget_.exact(initial_, member_count_, origin_->binding_->capture_.retained_.count == 4,
            origin_->binding_->capture_.retained_.report.semantic.active_sequence); !status) return status;
        if (auto status = budget_.charges(extension_reads_, charged_, reads_); !status) return status;
        for (usize at = 0; at < members_.size(); ++at)
            if (members_[at].inode != seal_.members[at].inode || members_[at].generation != seal_.members[at].generation ||
                members_[at].next != seal_.members[at].next || members_[at].inode != origin_->members_[at].inode ||
                members_[at].generation != origin_->members_[at].generation || members_[at].next != origin_->members_[at].next)
                return Status::fault("composed original full member vector changed");
        for (usize at = 0; at < storage_->rows_.size(); ++at)
            if (!same_row(storage_->rows_[at], storage_->sealed_rows_[at]) || storage_->rows_[at].ready != (at < row_count_))
                return Status::fault("composed full immutable rows changed");
        if (extension_ && storage_->extension_ != storage_->sealed_extension_)
            return Status::fault("composed original ninth WAL observation changed");
        if (capture_ready_ && (capture_.composed_history_ != this || capture_.composed_origin_ != this ||
            !capture_.reclaim_session_anchor_.exact(capture_.session_) || capture_.session_ != original_session_ ||
            !capture_.namespace_storage_exact()))
            return Status::fault("composed retained ephemeral capture changed");
        return origin_->retained_exact();
    }
    [[nodiscard]] Status stable() const
    {
        if (auto status = retained_exact(); !status) return status;
        if (auto status = origin_->stable(); !status) return status;
        return reclaim_history::same_session(progress_, origin_->progress_) ? Status{} :
            Status::fault("composed shared original Session progress differs");
    }
    [[nodiscard]] Status in_flight_exact() const
    {
        if (auto status = retained_exact(); !status) return status;
        if (!reading_) return Status::fault("composed nested read is not in flight");
        if (auto status = origin_->in_flight_exact(); !status) return status;
        return reclaim_history::same_session(progress_, origin_->progress_) ? Status{} :
            Status::fault("composed paused original progress differs");
    }
    [[nodiscard]] Status adopt(const block::SessionReport &paused)
    {
        if (!reclaim_history::same_session(progress_, paused) || paused.reads == ~block::SessionCounter{0} || reads_ == ~block::SessionCounter{0})
            return Status::fault("composed paused read counters changed");
        auto expected = paused; ++expected.reads; ++expected.completed_reads;
        if (!reclaim_history::same_session(expected, origin_->progress_))
            return Status::fault("composed exact completion delta differs");
        progress_ = seal_.progress = expected; ++reads_; ++seal_.reads;
        return {};
    }
    enum class PrefixHead : u8 { predecessor, active, cleanup };
    // Private image-selection input, never authority or a progress receipt.
    // A future Owner seal/ledger must derive these values from paired successful
    // transport returns. No public report is accepted by this resolver.
    struct PhysicalPrefix
    {
        usize completed_rows{}, wal_writes{};
        u8 home_after_mask{};
        PrefixHead head{PrefixHead::predecessor};
    };
    [[nodiscard]] Status prefix_exact(const PhysicalPrefix &prefix) const
    {
        if (auto status = retained_exact(); !status) return status;
        if (!complete_ || ordinary_ || !endpoint_ || reading_ || prefix.completed_rows > row_count_)
            return Status::fault("composed physical prefix requires complete retained dry route");
        if (prefix.completed_rows == row_count_)
            return !prefix.wal_writes && !prefix.home_after_mask && prefix.head == PrefixHead::predecessor ?
                Status{} : Status::fault("composed physical terminal prefix retains a current stage");
        const auto &row = storage_->rows_[prefix.completed_rows];
        const auto &plan = row.plan.reclaim;
        if (plan.count < 4 || plan.count > 5 || row.wal.count != plan.count)
            return Status::fault("composed physical prefix retained row width differs");
        const u8 full = u8((1U << plan.count) - 1);
        if (prefix.home_after_mask & ~full)
            return Status::fault("composed physical prefix home mask exceeds retained row");
        // Allocation mixtures are legal only after the tombstone. The final
        // orphan-head image requires every preceding home, independent of count.
        if ((prefix.home_after_mask & u8(full & ~1U)) && !(prefix.home_after_mask & 1U))
            return Status::fault("composed physical prefix allocation precedes tombstone");
        if ((prefix.home_after_mask & (1U << (plan.count - 1))) && prefix.home_after_mask != full)
            return Status::fault("composed physical prefix orphan head precedes allocations");
        if (!prefix.completed_rows)
        {
            const auto &capture = origin_->binding_->capture_.retained_;
            u8 original_mask{};
            for (usize home = 0; home < plan.count; ++home)
                if (capture.current[home] == plan.after[home]) original_mask |= u8(1U << home);
                else if (capture.current[home] != plan.before[home])
                    return Status::fault("composed physical prefix original home is not retained before/after");
            if (prefix.wal_writes || prefix.head == PrefixHead::predecessor ||
                (prefix.home_after_mask & original_mask) != original_mask)
                return Status::fault("composed physical prefix changes the selected original active WAL or homes");
        }
        else
        {
            // Publication order is descriptor, payloads, end, commit, active head.
            // The untouched ninth block must still come from the last wide row.
            if (prefix.wal_writes > row.wal.count + 3 ||
                (prefix.head != PrefixHead::predecessor && prefix.wal_writes != row.wal.count + 3) ||
                (prefix.home_after_mask && prefix.head == PrefixHead::predecessor))
                return Status::fault("composed physical prefix publication stage differs");
        }
        if (prefix.head != PrefixHead::predecessor && prefix.head != PrefixHead::active && prefix.head != PrefixHead::cleanup)
            return Status::fault("composed physical prefix head selector differs");
        if (prefix.head == PrefixHead::cleanup && prefix.home_after_mask != full)
            return Status::fault("composed physical prefix cleanup precedes complete homes");
        return {};
    }
    // Select exact completed-row images plus only the current returned writes.
    // Unlike resolve(), this never overlays an unexecuted future dry row and
    // never calls readonly stable() after the original Owner starts mutation.
    [[nodiscard]] Status resolve_physical_prefix(const PhysicalPrefix &prefix, u32 physical, const Block *&out) const
    {
        out = nullptr;
        if (auto status = prefix_exact(prefix); !status) return status;
        return resolve_retained_physical_prefix(prefix, physical, out);
    }
    // Borrowed only after the same Owner has checked the complete retained
    // history. This pure lookup invokes neither I/O nor an external callback.
    [[nodiscard]] Status resolve_retained_physical_prefix(const PhysicalPrefix &prefix, u32 physical, const Block *&out) const
    {
        out = nullptr;
        const Block *expected{};
        if (extension_ && physical == origin_->binding_->journal_first_ + 8) expected = &storage_->extension_;
        else if (auto status = origin_->original(physical, expected); !status) return status;
        const u32 first = origin_->binding_->journal_first_;
        for (usize index = 0; index < prefix.completed_rows; ++index)
        {
            const auto &row = storage_->rows_[index];
            for (usize home = 0; home < row.plan.reclaim.count; ++home)
                if (physical == row.plan.reclaim.targets[home]) expected = &row.plan.reclaim.after[home];
            if (physical == first) expected = &row.cleanup;
            if (index)
            {
                if (physical == first + 1) expected = &row.wal.descriptor;
                for (usize payload = 0; payload < row.wal.count; ++payload)
                    if (physical == first + 2 + payload) expected = &row.wal.payloads[payload];
                if (physical == first + 2 + row.wal.count) expected = &row.wal.commit;
                if (physical == first + 3 + row.wal.count) expected = &row.wal.end;
            }
        }
        if (prefix.completed_rows < row_count_)
        {
            const auto &row = storage_->rows_[prefix.completed_rows];
            for (usize home = 0; home < row.plan.reclaim.count; ++home)
                if ((prefix.home_after_mask & (1U << home)) && physical == row.plan.reclaim.targets[home])
                    expected = &row.plan.reclaim.after[home];
            if (prefix.completed_rows)
            {
                if (prefix.wal_writes && physical == first + 1) expected = &row.wal.descriptor;
                for (usize payload = 0; payload < row.wal.count; ++payload)
                    if (prefix.wal_writes > payload + 1 && physical == first + 2 + payload)
                        expected = &row.wal.payloads[payload];
                if (prefix.wal_writes > row.wal.count + 1 && physical == first + 3 + row.wal.count)
                    expected = &row.wal.end;
                if (prefix.wal_writes > row.wal.count + 2 && physical == first + 2 + row.wal.count)
                    expected = &row.wal.commit;
            }
            if (physical == first && prefix.head == PrefixHead::active) expected = &row.wal.journal_head;
            if (physical == first && prefix.head == PrefixHead::cleanup) expected = &row.cleanup;
        }
        out = expected;
        return {};
    }
    [[nodiscard]] Status resolve(u32 physical, const Block *&out) const
    {
        if (extension_ && physical == origin_->binding_->journal_first_ + 8) out = &storage_->extension_;
        else if (auto status = origin_->original(physical, out); !status) return status;
        const u32 first = origin_->binding_->journal_first_;
        for (usize index = 0; index < row_count_; ++index)
        {
            const auto &row = storage_->rows_[index];
            for (usize home = 0; home < row.plan.reclaim.count; ++home)
                if (physical == row.plan.reclaim.targets[home]) out = &row.plan.reclaim.after[home];
            if (physical == first) out = &row.cleanup;
            if (index)
            {
                if (physical == first + 1) out = &row.wal.descriptor;
                for (usize payload = 0; payload < row.wal.count; ++payload)
                    if (physical == first + 2 + payload) out = &row.wal.payloads[payload];
                if (physical == first + 2 + row.wal.count) out = &row.wal.commit;
                if (physical == first + 3 + row.wal.count) out = &row.wal.end;
            }
        }
        return {};
    }
    void begin_phase(ReadPhase phase)
    { phase_ = seal_.phase = phase; phase_reads_ = seal_.phase_reads = 0; }
    block::SessionCounter phase_limit(ReadPhase phase) const { return budget_.per_pass(phase); }
    block::SessionCounter phase_total_limit(ReadPhase phase) const
    {
        return budget_.cumulative(phase);
    }
    [[nodiscard]] Status read_blocks(u64 first, std::span<std::byte> bytes)
    {
        UnguardedRead guard;
        return read_blocks(first, bytes, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status read_blocks(u64 first, std::span<std::byte> bytes, Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        if (first % 2 || first >= 65536 || bytes.size() != 1024) return reject(Status::invalid_argument("composed read bounds"));
        const usize phase = usize(phase_);
        if (phase >= charged_.size() || phase_reads_ >= phase_limit(phase_) || charged_[phase] >= phase_total_limit(phase_))
            return reject(Status::overflow("composed named phase READ allowance exhausted before dispatch"));
        const u32 physical = u32(first / 2); const Block *expected{};
        if (auto status = resolve(physical, expected); !status) return reject(status);
        const auto paused = progress_; const auto completion = completion_.report();
        reading_ = seal_.reading = true;
        const auto status = extension_ && physical == origin_->binding_->journal_first_ + 8 ?
            origin_->extension_read(bytes, &storage_->sealed_extension_, guard) : origin_->read_blocks(first, bytes, guard);
        if (!status) return reject(status);
        if (auto guarded = retained_exact(); !guarded) return reject(guarded);
        if (ordinary_ && !same_completion(completion, completion_.report()))
            return reject(Status::fault("composed ordinary callback changed paused report"));
        if (auto guarded = adopt(paused); !guarded) return reject(guarded);
        ++phase_reads_; ++seal_.phase_reads; ++charged_[phase]; ++seal_.charged[phase];
        for (usize at = 0; at < bytes.size(); ++at) bytes[at] = std::byte((*expected)[at]);
        reading_ = seal_.reading = false;
        return stable();
    }
    [[nodiscard]] Status compare_all()
    {
        UnguardedRead guard;
        return compare_all(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_all(Guard &guard)
    {
        usize observed{};
        const auto visit = [&](u32 physical, const Block &) -> Status {
            if (++observed > 2335) return Status::overflow("composed origin enumeration exceeds H/J allowance");
            return read_blocks(u64{physical} * 2, std::as_writable_bytes(std::span{storage_->checking_}), guard);
        };
        if (auto status = origin_->each_original(visit); !status) return reject(status);
        if (extension_)
            if (auto status = visit(origin_->binding_->journal_first_ + 8, storage_->extension_); !status) return reject(status);
        last_enumeration_ = observed;
        return stable();
    }
    [[nodiscard]] Status compare_capture(const Capture &capture)
    {
        UnguardedRead guard;
        return compare_capture(capture, guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status compare_capture(const Capture &capture, Guard &guard)
    {
        usize observed{};
        const auto compare = [&](u32 physical, const Block &expected) -> Status {
            if (++observed > 2327) return Status::overflow("composed dry plan enumeration exceeds K allowance");
            if (auto status = read_blocks(u64{physical} * 2, std::as_writable_bytes(std::span{storage_->checking_}), guard); !status) return status;
            return storage_->checking_ == expected ? Status{} : Status::fault("composed retained projected capture differs");
        };
        const auto &images = *capture.storage_;
        if (auto status = compare(1, images.superblock); !status) return status;
        if (auto status = compare(2, images.descriptors); !status) return status;
        if (auto status = compare(u32(capture.binding_.layout.journal_first_block), images.journal); !status) return status;
        for (usize group = 0; group < 4; ++group)
        {
            if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 1].first), images.block_bitmaps[group]); !status) return status;
            if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 2].first), images.inode_bitmaps[group]); !status) return status;
            if (capture.initialized_[group])
                for (usize at = 0; at < 512; ++at)
                    if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 3].first) + u32(at), images.inode_tables[group][at]); !status) return status;
        }
        for (usize at = 0; at < capture.directory_count_; ++at)
            if (auto status = compare(images.directories[at].physical, images.directories[at].bytes); !status) return status;
        for (usize at = 0; at < capture.bootstrap_count_; ++at)
            if (auto status = compare(images.bootstrap_reads[at].physical, images.bootstrap_reads[at].bytes); !status) return status;
        return stable();
    }
    [[nodiscard]] Status check_row(Row &row)
    {
        const auto &plan = row.plan.reclaim; const auto &member = members_[index_];
        if (!row.plan.valid || !row.wal.ready || plan.inode != member.inode || plan.old_generation != member.generation ||
            plan.next_head != member.next || plan.retained_generation != plan.old_generation + 1 ||
            plan.old_generation == ~u32{0} || (plan.count != 4 && plan.count != 5) || row.wal.count != plan.count ||
            row.wal.targets != plan.targets || row.wal.sequence != storage_->rows_[row_count_ - 1].cleanup_sequence + 1)
            return Status::fault("composed dry row original identity differs");
        const u32 first = origin_->binding_->journal_first_;
        for (usize home = 0; home < plan.count; ++home)
        {
            const u32 physical = plan.targets[home]; const Block *before{};
            if (physical >= 32768 || (physical >= first && physical - first < origin_->binding_->journal_blocks_))
                return Status::fault("composed dry row overlaps WAL or volume bounds");
            for (usize earlier = 0; earlier < home; ++earlier)
                if (plan.targets[earlier] == physical) return Status::fault("composed duplicate home");
            if (auto status = resolve(physical, before); !status) return status;
            if (*before != plan.before[home]) return Status::fault("composed whole before differs from preceding terminal");
        }
        for (usize data = 0; data < plan.released_blocks; ++data)
            for (usize earlier = 0; earlier < row_count_; ++earlier)
                for (usize previous = 0; previous < storage_->rows_[earlier].plan.reclaim.released_blocks; ++previous)
                    if (plan.released_mapping[data] == storage_->rows_[earlier].plan.reclaim.released_mapping[previous])
                        return Status::fault("composed release mappings overlap");
        row.cleanup = row.wal.journal_head; row.cleanup_sequence = row.wal.sequence + 2;
        be32(row.cleanup, 24, row.cleanup_sequence); be32(row.cleanup, 28, 0); be32(row.cleanup, 252, 0);
        be32(row.cleanup, 252, jbd2::detail::crc32c(~u32{0}, row.cleanup));
        const auto codec = jbd2::Codec::open(row.cleanup, origin_->binding_->journal_blocks_, origin_->binding_->uuid_,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec || codec.value().superblock().start_block || codec.value().superblock().sequence != row.cleanup_sequence)
            return Status::fault("composed dry cleanup head differs");
        row.inode = member.inode; row.generation = member.generation; row.next = member.next; row.index = index_; row.ready = true;
        return {};
    }
    [[nodiscard]] Status run()
    {
        UnguardedRead guard;
        return run(guard);
    }
    template <typename Guard> requires block::ReadDispatchGuard<Guard, Session>
    [[nodiscard]] Status run(Guard &guard)
    {
        if (auto status = stable(); !status) return reject(status);
        if (complete_) return Status::already_exists("composed complete dry route is one-shot");
        while (index_ < member_count_)
        {
            // One ephemeral pair, one-shot capture instances, no reset of admitted origin objects.
            capture_ready_ = seal_.capture = false;
            capture_.~Capture(); ::new (static_cast<void *>(&capture_)) Capture{};
            storage_->capture_storage_.~Ext4OrphanChainCaptureStorage();
            ::new (static_cast<void *>(&storage_->capture_storage_)) Ext4OrphanChainCaptureStorage{};
            storage_->capture_seal_.~Ext4NamespaceSnapshotSeal();
            ::new (static_cast<void *>(&storage_->capture_seal_)) Ext4NamespaceSnapshotSeal{};
            begin_phase(ReadPhase::capture);
            if (auto status = capture_.capture_composed_result(*original_session_, storage_->capture_storage_, storage_->scratch_,
                *this, storage_->capture_seal_, guard); !status) return reject(status);
            capture_ready_ = seal_.capture = true;
            auto expected = origin_->binding_->capture_.retained_.binding;
            expected.empty_log = true; expected.journal_sequence = storage_->rows_[row_count_ - 1].cleanup_sequence;
            if (!reclaim_history::same_info(capture_.binding_, expected) || capture_.chain_count_ != member_count_ - index_ ||
                capture_.report_.head != members_[index_].inode || capture_.report_.generation != members_[index_].generation)
                return reject(Status::fault("composed remaining chain binding or count differs"));
            for (usize at = 0; at < capture_.chain_count_; ++at)
                if (capture_.storage_->chain[at].number != members_[index_ + at].inode ||
                    capture_.storage_->chain[at].expected_generation != members_[index_ + at].generation)
                    return reject(Status::fault("composed immutable remaining member differs"));
            auto &row = storage_->pending_; row.~Row(); ::new (static_cast<void *>(&row)) Row{};
            begin_phase(ReadPhase::plan);
            if (auto status = capture_.dry_plan(*this, row.plan, guard); !status) return reject(status);
            const auto &image = capture_.storage_->chain[0];
            const auto inode = std::span<const u8>{image.table}.subspan(((image.number - 1) % 4) * 256, 256);
            const u32 extra = ext4_checked::le32(inode, 132);
            row.seconds = ext4_checked::le32(inode, 12); row.nanoseconds = extra >> 2;
            if ((extra & 3) || row.seconds > 0x7fffffffU || row.nanoseconds >= 1000000000U)
                return reject(Status::unsupported("composed original member ctime"));
            const Block *head{};
            if (auto status = resolve(origin_->binding_->journal_first_, head); !status) return reject(status);
            // From pure dry-plan return through row sealing there is no transport or external callback.
            if (auto status = Ext4OrphanReclaimJournalPlanner::plan(expected, row.plan.reclaim, *head, row.seconds,
                row.nanoseconds, storage_->formatter_, row.wal); !status) return reject(status);
            if (auto status = capture_.namespace_storage_exact(); !status) return reject(status);
            if (auto status = check_row(row); !status) return reject(status);
            storage_->rows_[row_count_] = storage_->sealed_rows_[row_count_] = row;
            ++row_count_; ++index_; seal_.rows = row_count_; seal_.index = index_;
        }
        // Full ordinary prepare includes allocated inode and nested directory validation.
        begin_phase(ReadPhase::ordinary);
        OrdinaryView<Guard> view(*this, guard); ordinary_ = seal_.ordinary = true;
        const auto status = completion_.prepare(view, {0, original_session_->geometry().block_count});
        ordinary_ = seal_.ordinary = false;
        if (!status) return reject(status);
        const auto &report = completion_.report();
        if (report.phase != Ext4CompletionPhase::preflight || !report.empty_log || report.write_attempts || report.flush_attempts ||
            report.journal_sequence != storage_->rows_[row_count_ - 1].cleanup_sequence)
            return reject(Status::fault("composed full ordinary endpoint differs"));
        terminal_report_ = report; endpoint_ = seal_.endpoint = true;
        begin_phase(ReadPhase::terminal);
        if (auto checked = compare_all(guard); !checked) return checked;
        if (member_count_ && !capture_.namespace_storage_exact())
            return reject(Status::fault("composed last ephemeral result changed during terminal reads"));
        complete_ = seal_.complete = true;
        return stable();
    }
    reclaim_history::SessionAnchor<Session> session_anchor_{};
    Ext4ReclaimComposedReadBudget budget_{};
    Origin *origin_{};
    Ext4ReclaimComposedStorage *storage_{};
    Session *original_session_{};
    std::array<Member, 7> members_{};
    block::SessionReport initial_{}, progress_{};
    usize member_count_{}, index_{}, row_count_{}, last_enumeration_{};
    block::SessionCounter reads_{};
    Seal seal_{};
    ReadPhase phase_{ReadPhase::unused};
    std::array<block::SessionCounter, 4> charged_{};
    block::SessionCounter phase_reads_{}, extension_reads_{};
    Capture capture_{};
    Completion completion_{false, true};
    Ext4CompletionReport terminal_report_{};
    bool attempted_{}, ready_{}, rejected_{}, reading_{}, extension_{}, complete_{}, ordinary_{}, endpoint_{}, capture_ready_{};
};

// Cold readonly registration policy delegates to the same complete capture
// mechanism used by a live owner's existing session. Only this wrapper owns
// acquisition and closure; it never upgrades the session to writable access.
template <usize DeviceCapacity = 4>
class Ext4OrphanSnapshotAdmission final
{
    using Registry = block::BlockSessions<DeviceCapacity>;
  public:
    Ext4OrphanSnapshotAdmission() = default;
    Ext4OrphanSnapshotAdmission(const Ext4OrphanSnapshotAdmission &) = delete;
    Ext4OrphanSnapshotAdmission &operator=(const Ext4OrphanSnapshotAdmission &) = delete;
    Ext4OrphanSnapshotAdmission(Ext4OrphanSnapshotAdmission &&) = delete;
    Ext4OrphanSnapshotAdmission &operator=(Ext4OrphanSnapshotAdmission &&) = delete;

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
        block::SessionBounds bounds, Ext4OrphanSnapshotStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch)
    {
        if (attempted_) return Status::already_exists("orphan snapshot is one-shot");
        attempted_ = true; report_.phase = Ext4OrphanSnapshotPhase::opening;
        if (auto status = registry.acquire(key, bounds, block::SessionAccess::readonly, session_); !status)
        {
            report_.phase = Ext4OrphanSnapshotPhase::rejected; report_.status = status;
            return status;
        }
        delegated_ = true;
        return capture_.capture(session_, storage, scratch);
    }
    [[nodiscard]] Status plan_head(u32 head, u32 generation, Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        output = {};
        if (!delegated_) return Status::not_initialized("orphan snapshot is not admitted");
        return capture_.plan_head(head, generation, output);
    }
    [[nodiscard]] Status close()
    {
        if (report().phase != Ext4OrphanSnapshotPhase::admitted)
            return Status::busy("orphan snapshot cannot close in current phase");
        if (auto status = session_.close(); !status) return capture_.reject(status);
        report_ = capture_.report(); report_.valid = false;
        report_.phase = Ext4OrphanSnapshotPhase::closed; delegated_ = false;
        return {};
    }
    [[nodiscard]] const Ext4OrphanSnapshotReport &report() const
    { return delegated_ ? capture_.report() : report_; }
  private:
    typename Registry::Session session_{};
    Ext4OrphanChainCapture<DeviceCapacity> capture_{};
    Ext4OrphanSnapshotReport report_{};
    bool attempted_{}, delegated_{};
};
} // namespace ok::fs
