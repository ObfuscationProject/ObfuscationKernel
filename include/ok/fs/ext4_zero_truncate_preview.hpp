#pragma once

#include "ok/fs/ext4_zero_truncate_witness.hpp"

namespace ok::fs
{
struct Ext4ZeroTruncateJournalRecords
{
    const Ext4ZeroTruncateBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4ZeroTruncateBlock> payloads{};
};
struct Ext4ZeroTruncateJournalScratch
{
    Ext4ZeroTruncateWitnessScratch witness{};
    Ext4ZeroTruncatePlan decoded{};
    std::array<Ext4ZeroTruncateBlock, 4> after{};
    std::array<jbd2::ReplayAction, 4> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4ZeroTruncateBlock record{}, payload{};
};
struct Ext4ZeroTruncateJournalResult
{
    Ext4ZeroTruncateWitness witness{};
    u32 active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool complete{};
};

// Complete bounded journal and exact participant-image facts. These borrowing
// interfaces never authenticate a device snapshot or grant write authority.
class Ext4ZeroTruncateJournalVerifier final
{
    template <jbd2::BlockReader> friend class Ext4ZeroTruncatePreview;
    using Block = Ext4ZeroTruncateBlock;
    struct Reader
    {
        const Ext4ZeroTruncateJournalRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("zero truncate journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < 6) image = &records.payloads[number - 2];
            else if (number == 6) image = &records.commit;
            else if (number == 7) image = &records.end;
            if (!image) return Status::io_error("zero truncate journal fixed record bounds");
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = (*image)[byte];
            return {};
        }
    };
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] static bool overlaps(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static Status parse(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateJournalRecords &records, Ext4ZeroTruncateJournalScratch &scratch)
    {
        if (!binding.needs_recovery || binding.empty_log || records.payloads.size() != 4 ||
            binding.layout.block_size != 1024 ||
            !binding.structural_metadata().valid())
            return Status::invalid_argument("zero truncate active journal profile");
        if (!jbd2::detail::all_zero(records.end))
            return Status::io_error("zero truncate explicit end record is not zero");
        const auto opened = jbd2::Codec::open(records.head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!opened) return opened.status();
        const auto &head = opened.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.max_blocks != binding.layout.journal_blocks || head.max_blocks <= 7 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("zero truncate active sequence or fixed journal ring");
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(opened.value(), reader, binding.structural_metadata(),
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &plan = parsed.value();
        if (plan.empty_log || plan.incomplete_tail || plan.committed_transactions != 1 ||
            plan.revoked_actions || plan.revoke_records || plan.actions.size() != 4 || plan.next_sequence != head.sequence + 1)
            return Status::io_error("zero truncate complete committed journal grammar");
        for (usize home = 0; home < 4; ++home)
        {
            const auto &action = plan.actions[home];
            if (action.sequence != head.sequence || action.transaction_order || action.journal_block != home + 2 ||
                action.target_block != ext4_checked::le32(records.descriptor, 188 + home * 4))
                return Status::io_error("zero truncate exact journal action identity");
            scratch.after[home] = records.payloads[home];
            if (action.restore_magic) be32(scratch.after[home], 0, jbd2::magic);
        }
        return {};
    }
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4ZeroTruncateJournalRecords &records, const Block &inode_bitmap,
        std::span<const Block> actual_homes, Ext4ZeroTruncateJournalScratch &scratch,
        Ext4ZeroTruncateJournalResult &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(output, binding) ||
            overlap(scratch, records) || overlap(output, records) || overlap(scratch, inode_bitmap) ||
            overlap(output, inode_bitmap))
            return Status::invalid_argument("zero truncate journal request/workspace overlap");
        for (const auto *block : {&records.head, &records.descriptor, &records.commit, &records.end})
            if (overlap(scratch, *block) || overlap(output, *block))
                return Status::invalid_argument("zero truncate journal record/workspace overlap");
        for (const auto images : {records.payloads, actual_homes})
            if (overlaps(&scratch, sizeof(scratch), images.data(), images.size_bytes()) ||
                overlaps(&output, sizeof(output), images.data(), images.size_bytes()))
                return Status::invalid_argument("zero truncate journal image/workspace overlap");
        scratch = {}; output = {};
        if (actual_homes.size() != 4)
            return Status::invalid_argument("zero truncate actual whole-home count");
        if (auto status = parse(binding, records, scratch); !status) return status;
        Ext4ZeroTruncateWitness witness{};
        if (auto status = Ext4ZeroTruncateWitnessCodec::decode(binding, records.descriptor, scratch.after,
                inode_bitmap, scratch.witness, scratch.decoded, witness); !status) return status;
        if (witness.sequence != binding.journal_sequence)
            return Status::io_error("zero truncate witness active sequence differs");
        // A committed timestamp must describe this exact transform. Require the
        // private synchronous commit profile, including every unused byte.
        scratch.record = {};
        be32(scratch.record, 0, jbd2::magic); be32(scratch.record, 4, u32(jbd2::RecordType::commit));
        be32(scratch.record, 8, witness.sequence);
        be32(scratch.record, 52, witness.update.seconds); be32(scratch.record, 56, witness.update.nanoseconds);
        be32(scratch.record, 16, jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), scratch.record));
        if (scratch.record != records.commit)
            return Status::io_error("zero truncate exact commit image differs");
        u8 mask{};
        if (auto status = Ext4ZeroTruncateHomeMatcher::classify(scratch.decoded,
                std::span<const Block, 4>{actual_homes.data(), 4}, mask); !status) return status;
        output.witness = witness; output.active_sequence = witness.sequence;
        output.next_sequence = witness.sequence + 1; output.after_mask = mask; output.complete = true;
        return {};
    }
};

template <jbd2::BlockReader Reader> class Ext4ZeroTruncatePreview;

// Permanent one-shot scratch claim. This storage and its supplied transport
// must outlive the preview and remain exclusive; allocate outside kernel stacks.
template <jbd2::BlockReader Reader>
class Ext4ZeroTruncatePreviewWorkspace final
{
    friend class Ext4ZeroTruncatePreview<Reader>;
    template <typename> friend class Ext4ZeroTruncateTransaction;
    using Block = Ext4ZeroTruncateBlock;
    struct Overlay
    {
        Ext4ZeroTruncatePreviewWorkspace *workspace;
        bool final;
        Status read_block(u32 number, std::span<u8> out)
        {
            if (!workspace->journal_ready_ || out.size() < 1024)
                return Status::not_initialized("zero truncate ownership overlay is unavailable");
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
    Ext4ZeroTruncatePreviewWorkspace() = default;
    Ext4ZeroTruncatePreviewWorkspace(const Ext4ZeroTruncatePreviewWorkspace &) = delete;
    Ext4ZeroTruncatePreviewWorkspace &operator=(const Ext4ZeroTruncatePreviewWorkspace &) = delete;
    Ext4ZeroTruncatePreviewWorkspace(Ext4ZeroTruncatePreviewWorkspace &&) = delete;
    Ext4ZeroTruncatePreviewWorkspace &operator=(Ext4ZeroTruncatePreviewWorkspace &&) = delete;
  private:
    Status read(u32 number, std::span<u8> out)
    {
        if (!claimant_ || !reader_ || out.size() != 1024 || number >= binding_.layout.filesystem_blocks)
            return Status::invalid_argument("zero truncate preview transport read bounds");
        if (read_attempts_ == ~usize{0}) return Status::overflow("zero truncate preview read counter exhausted");
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
    Block head_{}, descriptor_{}, commit_{}, end_{}, inode_bitmap_{}, checked_{};
    std::array<Block, 4> payloads_{}, homes_{};
    Ext4ZeroTruncateJournalScratch journal_{};
    Ext4ZeroTruncateJournalResult result_{};
    Ext4ZeroTruncateOwnershipScratch ownership_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4ZeroTruncateOwnership old_proof_{}, final_proof_{};
    Overlay old_reader_{this, false}, final_reader_{this, true};
    Ext4ReadOnlyView<Overlay> old_view_{}, final_view_{};
};

enum class Ext4ZeroTruncatePreviewPhase : u8 { unused, capturing, journal, ownership, readback, ready, failed };
struct Ext4ZeroTruncatePreviewReport
{
    Ext4ZeroTruncatePreviewPhase phase{Ext4ZeroTruncatePreviewPhase::unused};
    Status status{};
    usize read_attempts{}, completed_reads{};
    u32 inode{}, generation{}, released_count{}, active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool old_ownership{}, final_ownership{}, snapshot_readback{};
};

// Read-only typed preview from an exclusive caller-protected transport. It
// captures the complete journal and homes, proves full old/final ownership via
// exact overlays, then rechecks captured bytes. No Session or replay permission
// is manufactured. A canonical mounted owner and counter reservation are later.
template <jbd2::BlockReader Reader>
class Ext4ZeroTruncatePreview final
{
    template <typename> friend class Ext4ZeroTruncateTransaction;
    using Workspace = Ext4ZeroTruncatePreviewWorkspace<Reader>;
    using Block = Ext4ZeroTruncateBlock;
  public:
    Ext4ZeroTruncatePreview() = default;
    Ext4ZeroTruncatePreview(const Ext4ZeroTruncatePreview &) = delete;
    Ext4ZeroTruncatePreview &operator=(const Ext4ZeroTruncatePreview &) = delete;
    Ext4ZeroTruncatePreview(Ext4ZeroTruncatePreview &&) = delete;
    Ext4ZeroTruncatePreview &operator=(Ext4ZeroTruncatePreview &&) = delete;
    [[nodiscard]] const Ext4ZeroTruncatePreviewReport &report() const { return report_; }
    [[nodiscard]] Status prepare(Reader &reader, const Ext4RecoveryInfo &binding, Workspace &workspace)
    {
        if (report_.phase != Ext4ZeroTruncatePreviewPhase::unused)
            return Status::already_exists("zero truncate preview is one-shot");
        report_.phase = Ext4ZeroTruncatePreviewPhase::capturing;
        const auto overlap = [](const void *a, usize a_size, const void *b, usize b_size)
        {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < a_size : one - two < b_size;
        };
        if (overlap(this, sizeof(*this), &workspace, sizeof(workspace)) ||
            overlap(&binding, sizeof(binding), &workspace, sizeof(workspace)) ||
            overlap(&reader, sizeof(reader), &workspace, sizeof(workspace)))
            return fail(Status::invalid_argument("zero truncate preview Owner/workspace overlap"));
        if (workspace.claimant_) return fail(Status::already_exists("zero truncate preview workspace already claimed"));
        workspace.claimant_ = this; workspace_ = &workspace;
        workspace.reader_ = &reader; workspace.binding_ = binding;
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks > binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return fail(Status::invalid_argument("zero truncate preview bounded active layout"));
        const u32 first = u32(binding.layout.journal_first_block);
        for (usize index = 0; index < 8; ++index)
            if (auto status = workspace.read(first + u32(index), record(index)); !status) return fail(status);
        report_.phase = Ext4ZeroTruncatePreviewPhase::journal;
        const Ext4ZeroTruncateJournalRecords records{workspace.head_, workspace.descriptor_, workspace.commit_, workspace.end_, workspace.payloads_};
        if (auto status = Ext4ZeroTruncateJournalVerifier::parse(workspace.binding_, records, workspace.journal_); !status)
            return fail(status);
        const u32 inode = ext4_checked::le32(workspace.descriptor_, 164);
        const u32 per_group = ext4_checked::le32(workspace.journal_.after[3], 40);
        if (!per_group || !inode || inode > ext4_checked::le32(workspace.journal_.after[3], 0) ||
            (inode - 1) / per_group >= binding.metadata_count / 4)
            return fail(Status::io_error("zero truncate preview captured inode geometry"));
        const auto bitmap = binding.metadata[((inode - 1) / per_group) * 4 + 2];
        if (bitmap.count != 1 || bitmap.first > 0xffffffffULL)
            return fail(Status::io_error("zero truncate preview inode bitmap binding"));
        workspace.inode_bitmap_target_ = u32(bitmap.first);
        if (auto status = workspace.read(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status)
            return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = workspace.read(u32(workspace.journal_.actions[home].target_block), workspace.homes_[home]); !status)
                return fail(status);
        if (auto status = Ext4ZeroTruncateJournalVerifier::verify(workspace.binding_, records,
                workspace.inode_bitmap_, workspace.homes_, workspace.journal_, workspace.result_); !status) return fail(status);
        workspace.journal_ready_ = true;
        report_.phase = Ext4ZeroTruncatePreviewPhase::ownership;
        if (auto status = workspace.old_view_.open(workspace.old_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.old_view_,
                workspace.result_.witness.update, workspace.ownership_, workspace.old_proof_); !status) return fail(status);
        const auto &plan = workspace.journal_.decoded;
        if (workspace.old_proof_.mapping != plan.released_mapping || workspace.old_proof_.count != plan.released_count ||
            workspace.old_proof_.group != plan.group)
            return fail(Status::io_error("zero truncate old overlay mapping differs"));
        report_.old_ownership = true;
        if (auto status = workspace.final_view_.open(workspace.final_reader_, workspace.binding_, workspace.opening_); !status) return fail(status);
        const Ext4ZeroTruncateUpdate empty{plan.inode, plan.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(workspace.binding_, workspace.final_view_,
                empty, workspace.ownership_, workspace.final_proof_); !status) return fail(status);
        if (workspace.final_proof_.count || workspace.final_proof_.inode != plan.inode ||
            workspace.final_proof_.generation != plan.generation)
            return fail(Status::io_error("zero truncate final overlay identity differs"));
        report_.final_ownership = true; report_.phase = Ext4ZeroTruncatePreviewPhase::readback;
        for (usize index = 0; index < 8; ++index)
            if (auto status = check(first + u32(index), record(index)); !status) return fail(status);
        if (auto status = check(workspace.inode_bitmap_target_, workspace.inode_bitmap_); !status) return fail(status);
        for (usize home = 0; home < 4; ++home)
            if (auto status = check(plan.targets[home], workspace.homes_[home]); !status) return fail(status);
        report_.snapshot_readback = true;
        report_.inode = plan.inode; report_.generation = plan.generation; report_.released_count = plan.released_count;
        report_.active_sequence = workspace.result_.active_sequence; report_.next_sequence = workspace.result_.next_sequence;
        report_.after_mask = workspace.result_.after_mask;
        report_.phase = Ext4ZeroTruncatePreviewPhase::ready; update_counts(); return {};
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
        return workspace_->checked_ == expected ? Status{} : Status::io_error("zero truncate captured source changed during preview");
    }
    void update_counts()
    {
        if (workspace_)
        { report_.read_attempts = workspace_->read_attempts_; report_.completed_reads = workspace_->completed_reads_; }
    }
    [[nodiscard]] Status fail(Status status)
    { report_.phase = Ext4ZeroTruncatePreviewPhase::failed; report_.status = status; update_counts(); return status; }
    Workspace *workspace_{};
    Ext4ZeroTruncatePreviewReport report_{};
};
} // namespace ok::fs
