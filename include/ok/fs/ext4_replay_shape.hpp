#pragma once

#include "ok/fs/ext4_recovery.hpp"
#include "ok/fs/ext4_directory_create_witness.hpp"

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
} // namespace ok::fs
