#pragma once

#include "ok/block/guarded_reads.hpp"
#include "ok/fs/ext4_orphan_directory_qualified_reclaim.hpp"
#include "ok/fs/ext4_namespace_result_history.hpp"
#include "ok/fs/ext4_reclaim_result_history.hpp"

namespace ok::fs
{
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
} // namespace ok::fs

#include "ok/fs/ext4_reclaim_composed_history.hpp"
