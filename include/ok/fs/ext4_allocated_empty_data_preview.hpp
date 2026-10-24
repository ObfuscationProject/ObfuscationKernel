#pragma once

#include "ok/fs/ext4_allocated_empty_data_journal_verify.hpp"
#include "ok/fs/ext4_allocated_empty_data_ownership.hpp"

namespace ok::fs
{
template <jbd2::BlockReader Reader> class Ext4AllocatedEmptyDataPreview;

// Permanent one-shot scratch claim. This storage and its supplied transport
// must outlive the preview and remain exclusive; allocate outside kernel stacks.
template <jbd2::BlockReader Reader>
class Ext4AllocatedEmptyDataPreviewWorkspace final
{
    friend class Ext4AllocatedEmptyDataPreview<Reader>;
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Block = Ext4FirstDataBlock;
    struct Overlay
    {
        Ext4AllocatedEmptyDataPreviewWorkspace *workspace;
        bool final;
        Status read_block(u32 number, std::span<u8> out)
        {
            if (!workspace->journal_ready_ || out.size() < 1024)
                return Status::not_initialized("allocated empty data ownership overlay is unavailable");
            for (usize home = 0; home < 4; ++home)
                if (number == workspace->journal_.decoded.targets[home])
                {
                    const auto &bytes = final ? workspace->journal_.decoded.after[home] : workspace->journal_.decoded.before[home];
                    for (usize byte = 0; byte < 1024; ++byte) out[byte] = bytes[byte];
                    return {};
                }
            return workspace->read(number, out.first(1024));
        }
    };
  public:
    Ext4AllocatedEmptyDataPreviewWorkspace() = default;
    Ext4AllocatedEmptyDataPreviewWorkspace(const Ext4AllocatedEmptyDataPreviewWorkspace &) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace &operator=(const Ext4AllocatedEmptyDataPreviewWorkspace &) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace(Ext4AllocatedEmptyDataPreviewWorkspace &&) = delete;
    Ext4AllocatedEmptyDataPreviewWorkspace &operator=(Ext4AllocatedEmptyDataPreviewWorkspace &&) = delete;
  private:
    Status read(u32 number, std::span<u8> out)
    {
        if (!claimant_ || !reader_ || out.size() != 1024 || number >= binding_.layout.filesystem_blocks)
            return Status::invalid_argument("allocated empty data preview transport read bounds");
        if (read_attempts_ == ~usize{0}) return Status::overflow("allocated empty data preview read counter exhausted");
        ++read_attempts_;
        if (auto status = reader_->read_block(number, out); !status) return status;
        ++completed_reads_; return {};
    }
    const void *claimant_{};
    Reader *reader_{};
    Ext4RecoveryInfo binding_{};
    usize read_attempts_{}, completed_reads_{};
    u32 inode_bitmap_target_{};
    bool journal_ready_{};
    Block head_{}, descriptor_{}, commit_{}, end_{}, inode_bitmap_{}, data_{}, checked_{};
    std::array<Block, 4> payloads_{}, homes_{};
    Ext4AllocatedEmptyDataJournalScratch journal_{};
    Ext4AllocatedEmptyDataJournalResult result_{};
    Ext4AllocatedEmptyDataOwnershipScratch old_ownership_{};
    Ext4ZeroTruncateOwnershipScratch final_ownership_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4AllocatedEmptyDataOwnership old_proof_{};
    Ext4ZeroTruncateOwnership final_proof_{};
    Overlay old_reader_{this, false}, final_reader_{this, true};
    Ext4ReadOnlyView<Overlay> old_view_{}, final_view_{};
};

enum class Ext4AllocatedEmptyDataPreviewPhase : u8 { unused, capturing, journal, ownership, readback, ready, failed };
struct Ext4AllocatedEmptyDataPreviewReport
{
    Ext4AllocatedEmptyDataPreviewPhase phase{Ext4AllocatedEmptyDataPreviewPhase::unused};
    Status status{};
    usize read_attempts{}, completed_reads{};
    u32 inode{}, generation{}, physical{}, group{}, size{}, active_sequence{}, next_sequence{};
    u64 old_version{}, new_version{};
    u8 after_mask{};
    bool old_ownership{}, final_ownership{}, snapshot_readback{};
};

// Read-only typed preview from an exclusive caller-protected transport. It
// captures the complete journal and homes, proves full old/final ownership via
// exact overlays, then rechecks captured bytes. No Session or replay permission
// is manufactured. A canonical mounted owner and counter reservation are later.
template <jbd2::BlockReader Reader>
class Ext4AllocatedEmptyDataPreview final
{
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Workspace = Ext4AllocatedEmptyDataPreviewWorkspace<Reader>;
    using Block = Ext4FirstDataBlock;
  public:
    Ext4AllocatedEmptyDataPreview() = default;
    Ext4AllocatedEmptyDataPreview(const Ext4AllocatedEmptyDataPreview &) = delete;
    Ext4AllocatedEmptyDataPreview &operator=(const Ext4AllocatedEmptyDataPreview &) = delete;
    Ext4AllocatedEmptyDataPreview(Ext4AllocatedEmptyDataPreview &&) = delete;
    Ext4AllocatedEmptyDataPreview &operator=(Ext4AllocatedEmptyDataPreview &&) = delete;
    [[nodiscard]] const Ext4AllocatedEmptyDataPreviewReport &report() const { return report_; }
    [[nodiscard]] Status prepare(Reader &reader, const Ext4RecoveryInfo &binding, Workspace &workspace)
    {
        if (report_.phase != Ext4AllocatedEmptyDataPreviewPhase::unused)
            return Status::already_exists("allocated empty data preview is one-shot");
        const auto overlap = [](const void *a, usize a_size, const void *b, usize b_size)
        {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < a_size : one - two < b_size;
        };
        // Refuse borrowing aliases before changing any potentially aliased byte.
        if (overlap(this, sizeof(*this), &workspace, sizeof(workspace)) ||
            overlap(this, sizeof(*this), &binding, sizeof(binding)) ||
            overlap(this, sizeof(*this), &reader, sizeof(reader)) ||
            overlap(&binding, sizeof(binding), &workspace, sizeof(workspace)) ||
            overlap(&reader, sizeof(reader), &workspace, sizeof(workspace)))
            return Status::invalid_argument("allocated empty data preview owner/workspace/input overlap");
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::capturing;
        if (workspace.claimant_) return fail(Status::already_exists("allocated empty data preview workspace already claimed"));
        workspace.claimant_ = this; workspace_ = &workspace;
        workspace.reader_ = &reader; workspace.binding_ = binding;
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks > binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return fail(Status::invalid_argument("allocated empty data preview bounded active layout"));
        const u32 first = u32(binding.layout.journal_first_block);
        for (usize index = 0; index < 8; ++index)
            if (auto status = workspace.read(first + u32(index), record(index)); !status) return fail(status);
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::journal;
        const Ext4AllocatedEmptyDataJournalRecords records{workspace.head_, workspace.descriptor_, workspace.commit_, workspace.end_, workspace.payloads_};
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::parse(workspace.binding_, records, workspace.journal_); !status)
            return fail(status);
        const u32 inode = ext4_checked::le32(workspace.descriptor_, 164);
        const u32 per_group = ext4_checked::le32(workspace.journal_.after[2], 40);
        if (!per_group || !inode || inode > ext4_checked::le32(workspace.journal_.after[2], 0) ||
            (inode - 1) / per_group >= binding.metadata_count / 4)
            return fail(Status::io_error("allocated empty data preview captured inode geometry"));
        const auto bitmap = binding.metadata[((inode - 1) / per_group) * 4 + 2];
        if (bitmap.count != 1 || bitmap.first > 0xffffffffULL)
            return fail(Status::io_error("allocated empty data preview inode bitmap binding"));
        workspace.inode_bitmap_target_ = u32(bitmap.first);
        if (auto status = workspace.read(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status)
            return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = workspace.read(u32(workspace.journal_.actions[home].target_block), workspace.homes_[home]); !status)
                return fail(status);
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::verify(workspace.binding_, records,
                workspace.inode_bitmap_, workspace.homes_, workspace.journal_, workspace.result_); !status) return fail(status);
        const auto &plan = workspace.journal_.decoded;
        if (auto status = workspace.read(plan.physical, workspace.data_); !status) return fail(status);
        workspace.journal_ready_ = true;
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::ownership;
        if (auto status = workspace.old_view_.open(workspace.old_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        if (auto status = Ext4AllocatedEmptyDataOwnershipValidator::prove(workspace.binding_, workspace.old_view_,
                plan, workspace.old_ownership_, workspace.old_proof_); !status) return fail(status);
        if (!workspace.old_proof_.complete || workspace.old_proof_.physical != plan.physical ||
            workspace.old_proof_.group != plan.group || workspace.old_proof_.old_version != plan.old_version)
            return fail(Status::io_error("allocated empty data old overlay candidate differs"));
        report_.old_ownership = true;
        if (auto status = workspace.final_view_.open(workspace.final_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        const Ext4ZeroTruncateUpdate filled{plan.update.inode, plan.update.generation, plan.update.size, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.final_view_,
                filled, workspace.final_ownership_, workspace.final_proof_); !status) return fail(status);
        if (workspace.final_proof_.count != 1 || workspace.final_proof_.mapping[0] != plan.physical ||
            workspace.final_proof_.group != plan.group || workspace.final_proof_.inode != plan.update.inode ||
            workspace.final_proof_.generation != plan.update.generation || workspace.final_proof_.old_size != plan.update.size)
            return fail(Status::io_error("allocated empty data final overlay mapping or identity differs"));
        report_.final_ownership = true; report_.phase = Ext4AllocatedEmptyDataPreviewPhase::readback;
        for (usize index = 0; index < 8; ++index)
            if (auto status = check(first + u32(index), record(index)); !status) return fail(status);
        if (auto status = check(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status) return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = check(plan.targets[home], workspace.homes_[home]); !status) return fail(status);
        if (auto status = check(plan.physical, workspace.data_); !status) return fail(status);
        report_.snapshot_readback = true;
        report_.inode = plan.update.inode; report_.generation = plan.update.generation;
        report_.physical = plan.physical; report_.group = plan.group; report_.size = plan.update.size;
        report_.old_version = plan.old_version; report_.new_version = plan.new_version;
        report_.active_sequence = workspace.result_.active_sequence; report_.next_sequence = workspace.result_.next_sequence;
        report_.after_mask = workspace.result_.after_mask;
        report_.phase = Ext4AllocatedEmptyDataPreviewPhase::ready; update_counts(); return {};
    }
  private:
    Block &record(usize index)
    {
        if (!index) return workspace_->head_;
        if (index == 1) return workspace_->descriptor_;
        if (index < 6) return workspace_->payloads_[index - 2];
        return index == 6 ? workspace_->commit_ : workspace_->end_;
    }
    [[nodiscard]] Status check(u32 number, const Block &expected)
    {
        if (auto status = workspace_->read(number, workspace_->checked_); !status) return status;
        return workspace_->checked_ == expected ? Status{} : Status::io_error("allocated empty data captured source changed during preview");
    }
    void update_counts()
    {
        if (workspace_)
        { report_.read_attempts = workspace_->read_attempts_; report_.completed_reads = workspace_->completed_reads_; }
    }
    [[nodiscard]] Status fail(Status status)
    { report_.phase = Ext4AllocatedEmptyDataPreviewPhase::failed; report_.status = status; update_counts(); return status; }
    Workspace *workspace_{};
    Ext4AllocatedEmptyDataPreviewReport report_{};
};
} // namespace ok::fs
