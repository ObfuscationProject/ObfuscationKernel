#pragma once

#include "ok/fs/ext4_owned_transaction.hpp"

namespace ok::fs
{
// Exclusive caller storage, outliving the Owner. Allocate outside kernel stacks.
// A claim is permanent even when acquisition or recovery fails. No storage,
// capability, live lifetime Ticket, or mutable execution plan is exported.
class Ext4EmptyOrphanRecoveryWorkspace final
{
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
  public:
    Ext4EmptyOrphanRecoveryWorkspace() = default;
    Ext4EmptyOrphanRecoveryWorkspace(const Ext4EmptyOrphanRecoveryWorkspace &) = delete;
    Ext4EmptyOrphanRecoveryWorkspace &operator=(const Ext4EmptyOrphanRecoveryWorkspace &) = delete;
    Ext4EmptyOrphanRecoveryWorkspace(Ext4EmptyOrphanRecoveryWorkspace &&) = delete;
    Ext4EmptyOrphanRecoveryWorkspace &operator=(Ext4EmptyOrphanRecoveryWorkspace &&) = delete;
  private:
    const void *claimant_{};
    smp::SpinLock lock_{};
    Ext4OrphanChainCaptureStorage original_{}, sealed_original_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch preparation_{};
    Ext4OrphanReclaimCaptureStorage observed_{};
    Ext4OrphanReclaimColdScratch verification_{};
    Ext4RecoveryBinding terminal_binding_{};
};

enum class Ext4EmptyOrphanRecoveryPhase : u8 { unused, opening, open, recovering, complete, closing, closed, rejected, failed };
struct Ext4EmptyOrphanRecoveryReport
{
    Ext4EmptyOrphanRecoveryPhase phase{Ext4EmptyOrphanRecoveryPhase::unused};
    Status status{};
    block::SessionReport original{}, observed{};
    u32 initial_head{}, final_sequence{};
    usize chain_members{}, reclaimed{}, released_blocks{}, verified_publications{};
    bool complete{};
};

// Fresh writable canonical Owner for a dirty EMPTY journal with an allocated
// orphan chain. Separate from live unlink lifetime authority. This object is
// unpublished until the full chain finishes; raw transport aliases and other
// storage users must remain excluded throughout its lifetime.
template <usize DeviceCapacity = 4>
class Ext4EmptyOrphanRecoveryOwner final
{
    using Owner = Ext4OwnedTransaction<DeviceCapacity, 8>;
    using Cache = typename Owner::Cache;
    using Block = Ext4OrphanBlock;
    struct ColdSeal final
    {
        ColdSeal() = default;
        ColdSeal(const ColdSeal &) = delete;
        ColdSeal &operator=(const ColdSeal &) = delete;
        const Ext4EmptyOrphanRecoveryOwner *owner{};
        const Owner *canonical_owner{};
        const Cache *cache{};
        const typename Cache::Registry::Session *session{};
        const Ext4EmptyOrphanRecoveryWorkspace *workspace{};
        const Ext4OrphanChainCaptureStorage *storage{};
        u64 identity{}, registration{}, session_serial{}, first_sector{}, sectors{}, domain{};
        u32 head{}, generation{}, sequence{}, next_head{};
        usize chain_members{};
        std::array<u8, 16> uuid{};
        bool active{};
    };
    class TerminalView final : public driver::BlockDevice
    {
      public:
        explicit TerminalView(Ext4EmptyOrphanRecoveryOwner &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.owner_.cache_.session_.geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        { Ext4DirectReadHooks hook; return read_blocks_guarded(first, out, hook); }
        template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Owner::Registry::Session>
        Status read_blocks_guarded(u64 first, std::span<std::byte> out, Hook &hook)
        {
            if (auto status = owner_.guard(); !status) return status;
            if (auto status = owner_.owner_.reclaim_session_exact(); !status) return status;
            auto &io = owner_.owner_.reclaim_io_;
            if (io.pending_writes || io.reads == ~block::SessionCounter{0} || first >= 65536 ||
                out.size() % 512 || out.size() / 512 > 65536 - first)
                return Status::overflow("empty orphan terminal bootstrap read bounds");
            auto authority = [this, original = &owner_]() -> Status
            {
                if (&owner_ != original) return Status::fault("empty orphan terminal Owner attachment changed");
                return original->guard();
            };
            if (auto status = guarded_ext4_direct_read(owner_.owner_.cache_.session_, io, authority, first, out, hook); !status)
                return status;
            if (auto status = owner_.owner_.reclaim_session_exact(); !status) return status;
            return owner_.guard();
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("empty orphan terminal view is readonly"); }
        Status flush() override { return Status::denied("empty orphan terminal view is readonly"); }
      private:
        Ext4EmptyOrphanRecoveryOwner &owner_;
    };
  public:
    using Registry = typename Owner::Registry;
    Ext4EmptyOrphanRecoveryOwner() = default;
    Ext4EmptyOrphanRecoveryOwner(const Ext4EmptyOrphanRecoveryOwner &) = delete;
    Ext4EmptyOrphanRecoveryOwner &operator=(const Ext4EmptyOrphanRecoveryOwner &) = delete;
    Ext4EmptyOrphanRecoveryOwner(Ext4EmptyOrphanRecoveryOwner &&) = delete;
    Ext4EmptyOrphanRecoveryOwner &operator=(Ext4EmptyOrphanRecoveryOwner &&) = delete;

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
        block::SessionBounds bounds, Ext4EmptyOrphanRecoveryWorkspace &workspace)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4EmptyOrphanRecoveryPhase::unused)
                return Status::already_exists("empty orphan Owner is one-shot");
            report_.phase = Ext4EmptyOrphanRecoveryPhase::opening;
        }
        const auto a = reinterpret_cast<usize>(this), b = reinterpret_cast<usize>(&workspace);
        if (a <= b ? b - a < sizeof(*this) : a - b < sizeof(workspace))
            return finish(Ext4EmptyOrphanRecoveryPhase::rejected, Status::invalid_argument("empty orphan workspace overlaps Owner"));
        {
            smp::ScopedSpinLock guard(workspace.lock_);
            if (workspace.claimant_)
                return finish(Ext4EmptyOrphanRecoveryPhase::rejected, Status::already_exists("empty orphan workspace already claimed"));
            workspace.claimant_ = this;
        }
        workspace_ = &workspace;
        if (auto status = owner_.open(registry, key, bounds); !status)
            return finish(Ext4EmptyOrphanRecoveryPhase::rejected, status);
        const auto initial = owner_.cache_.session_.report();
        if (!initial) return fail(initial.status());
        owner_.reclaim_io_ = initial.value();
        report_.original = report_.observed = initial.value();
        owner_.publication_domain_ = 1;
        return finish(Ext4EmptyOrphanRecoveryPhase::open, {});
    }

    [[nodiscard]] Status recover()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4EmptyOrphanRecoveryPhase::open)
                return Status::busy("empty orphan recovery is not admitted");
            report_.phase = Ext4EmptyOrphanRecoveryPhase::recovering;
        }
        u32 next{};
        for (usize member = 0; member < 8; ++member)
        {
            if (auto status = prepare(member, next); !status) return fail(status);
            if (auto status = publish(); !status) return fail(status);
            if (auto status = checkpoint(); !status) return fail(status);
            {
                smp::ScopedSpinLock guard(lock_);
                ++report_.reclaimed;
                report_.released_blocks += owner_.reclaim_plan_.reclaim.released_blocks;
                report_.observed = owner_.reclaim_io_;
                report_.final_sequence = jbd2::detail::be32(owner_.reclaim_empty_head_, 24);
            }
            expected_empty_head_ = owner_.reclaim_empty_head_;
            expected_directory_count_ = owner_.reclaim_disk_capture_.directory_count_;
            next = owner_.reclaim_plan_.reclaim.next_head;
            if (!next)
            {
                if (report_.reclaimed != report_.chain_members)
                    return fail(Status::fault("empty orphan initial chain count differs"));
                return finish(Ext4EmptyOrphanRecoveryPhase::complete, {});
            }
            if (member + 1 >= report_.chain_members || member + 1 == 8)
                return fail(Status::fault("empty orphan chain did not terminate"));
            // Reset only drained operation handles. Never reset cache slot
            // generations, Session counters, serials, quarantine, or workspace.
            const auto retained = owner_.reclaim_io_;
            if (auto status = owner_.rearm(); !status) return fail(status);
            owner_.reclaim_io_ = retained;
            if (owner_.publication_domain_ == ~u64{0})
                return fail(Status::overflow("empty orphan cold domain exhausted"));
            ++owner_.publication_domain_;
            seal_.active = false;
        }
        return fail(Status::fault("empty orphan bounded chain exhausted"));
    }
    [[nodiscard]] Status close()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4EmptyOrphanRecoveryPhase::complete &&
                report_.phase != Ext4EmptyOrphanRecoveryPhase::open)
                return Status::busy("empty orphan Owner cannot close in current phase");
            report_.phase = Ext4EmptyOrphanRecoveryPhase::closing;
        }
        if (auto status = owner_.close(); !status) return fail(status);
        seal_.active = false;
        return finish(Ext4EmptyOrphanRecoveryPhase::closed, {});
    }
    [[nodiscard]] Ext4EmptyOrphanRecoveryReport report() const
    { smp::ScopedSpinLock guard(lock_); return report_; }
    [[nodiscard]] MetadataOwnerReport buffers() const { return owner_.buffers(); }

  private:
    [[nodiscard]] Status finish(Ext4EmptyOrphanRecoveryPhase phase, Status status)
    {
        smp::ScopedSpinLock guard(lock_);
        report_.phase = phase; report_.status = status;
        report_.complete = phase == Ext4EmptyOrphanRecoveryPhase::complete ||
            (phase == Ext4EmptyOrphanRecoveryPhase::closed && report_.reclaimed && report_.reclaimed == report_.chain_members);
        return status;
    }
    [[nodiscard]] Status fail(Status status)
    {
        seal_.active = false;
        static_cast<void>(owner_.fail(status));
        const auto observed = owner_.cache_.session_.report();
        if (observed) { smp::ScopedSpinLock guard(lock_); report_.observed = observed.value(); }
        return finish(Ext4EmptyOrphanRecoveryPhase::failed, status);
    }
    [[nodiscard]] Status prepare(usize member, u32 expected_head)
    {
        if (!workspace_ || workspace_->claimant_ != this)
            return Status::fault("empty orphan original workspace claim differs");
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        auto &capture = owner_.reclaim_capture_;
        auto &storage = workspace_->original_;
        owner_.reclaim_storage_ = &storage; owner_.reclaim_scratch_ = &workspace_->preparation_;
        if (auto status = owner_.with_capture_read_budget([&](auto &guard) { return capture.capture(owner_.cache_.session_, storage, workspace_->preparation_, guard); }); !status) return status;
        const auto captured = capture.report();
        if (!Owner::exact_session(captured.original_io, owner_.reclaim_io_) ||
            !captured.valid || captured.chain_members == 0 || captured.chain_members > 8 ||
            (member && (captured.head != expected_head || captured.chain_members != report_.chain_members - member)))
            return Status::fault("empty orphan captured original identity differs");
        if (!member)
        {
            smp::ScopedSpinLock guard(lock_);
            report_.initial_head = captured.head; report_.chain_members = captured.chain_members;
            initial_uuid_ = capture.binding_.uuid;
            for (usize index = 0; index < captured.chain_members; ++index)
                initial_members_[index] = {storage.chain[index].number, storage.chain[index].expected_generation};
        }
        if (capture.binding_.uuid != initial_uuid_)
            return Status::fault("empty orphan initial filesystem identity differs");
        for (usize index = 0; index < captured.chain_members; ++index)
            if (storage.chain[index].number != initial_members_[member + index][0] ||
                storage.chain[index].expected_generation != initial_members_[member + index][1])
                return Status::fault("empty orphan initial remaining chain identity differs");
        if (member)
        {
            // The previous actual all-after capture is retained independently.
            // A new valid filesystem history cannot replace our own checkpoint.
            const auto &expected = workspace_->observed_;
            if (storage.superblock != expected.superblock || storage.descriptors != expected.descriptors ||
                storage.inode_bitmaps != expected.inode_bitmaps || storage.block_bitmaps != expected.block_bitmaps ||
                storage.journal != expected_empty_head_ || capture.directory_count_ != expected_directory_count_)
                return Status::fault("empty orphan next complete sources differ from own checkpoint");
            for (usize group = 0; group < 4; ++group)
                if (capture.initialized_[group] && storage.inode_tables[group] != expected.tables[group])
                    return Status::fault("empty orphan next whole tables differ from own checkpoint");
            for (usize index = 0; index < capture.directory_count_; ++index)
            {
                const auto &a = storage.directories[index], &b = expected.directories[index];
                if (a.inode != b.inode || a.logical != b.logical || a.physical != b.physical || a.bytes != b.bytes)
                    return Status::fault("empty orphan next directories differ from own checkpoint");
            }
        }
        if (auto status = owner_.with_capture_read_budget([&](auto &guard) { return capture.plan_head(captured.head, captured.generation, owner_.reclaim_plan_, guard); }); !status) return status;
        const auto &homes = owner_.reclaim_plan_.reclaim;
        const usize inode = ((homes.inode - 1) % 4) * 256;
        const u32 seconds = ext4_checked::le32(homes.before[0], inode + 12);
        const u32 extra = ext4_checked::le32(homes.before[0], inode + 132);
        if (seconds > 0x7fffffffU || (extra & 3) || (extra >> 2) >= 1000000000U)
            return Status::unsupported("empty orphan checked ctime outside journal profile");
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(capture.binding_, homes,
            storage.journal, seconds, extra >> 2, owner_.reclaim_format_, owner_.reclaim_journal_); !status) return status;
        Status status;
        { smp::ScopedSpinLock guard(owner_.cache_.lock_); status = owner_.reconcile_orphan_cache(); }
        if (!status) return status;
        std::array<u64, 16> released{};
        for (usize index = 0; index < homes.released_blocks; ++index) released[index] = homes.released_mapping[index];
        if (auto retired = owner_.cache_.retire_clean_blocks(std::span<const u64>{released}.first(homes.released_blocks)); !retired)
            return retired.status();
        owner_.participant_count_ = homes.count + 1;
        owner_.blocks_[0] = capture.binding_.layout.journal_first_block;
        owner_.sealed_[0] = owner_.reclaim_journal_.journal_head;
        for (usize index = 0; index < homes.count; ++index)
        { owner_.blocks_[index + 1] = homes.targets[index]; owner_.sealed_[index + 1] = homes.after[index]; }
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            if (status = capture.stable(); !status) return status;
            const auto before = owner_.cache_.session_.report(); if (!before) return before.status();
            if (status = owner_.cache_.pin(owner_.blocks_[index], owner_.pins_[index]); !status) return status;
            const auto after = owner_.cache_.session_.report(); if (!after) return after.status();
            if (after.value().reads < before.value().reads || after.value().reads - before.value().reads > 1 ||
                after.value().completed_reads != after.value().reads ||
                capture.report_.session_reads > ~block::SessionCounter{0} - (after.value().reads - before.value().reads))
                return Status::fault("empty orphan canonical pin exact read accounting");
            capture.report_.session_reads += after.value().reads - before.value().reads;
            if (status = capture.stable(); !status) return status;
            const auto &old = index ? homes.before[index - 1] : storage.journal;
            {
                smp::ScopedSpinLock guard(owner_.cache_.lock_);
                const auto *entry = owner_.cache_.find(owner_.pins_[index]);
                if (!entry || entry->report.block != owner_.blocks_[index] ||
                    entry->report.state != MetadataBufferState::ready || entry->report.staged_dirty)
                    return Status::fault("empty orphan canonical before identity differs");
                for (usize byte = 0; byte < 1024; ++byte)
                    if (entry->canonical[byte] != std::byte(old[byte]))
                        return Status::fault("empty orphan canonical before whole bytes differ");
            }
        }
        std::array<typename Owner::Pin *, Owner::participant_capacity> pins{};
        for (usize index = 0; index < owner_.participant_count_; ++index) pins[index] = &owner_.pins_[index];
        if (status = owner_.cache_.begin(std::span<typename Owner::Pin *const>{pins}.first(owner_.participant_count_), owner_.transaction_); !status)
            return status;
        for (usize index = 0; index < owner_.participant_count_; ++index)
            if (status = owner_.transaction_.edit(owner_.blocks_[index], 0,
                std::as_bytes(std::span{owner_.sealed_[index]})); !status) return status;
        if (status = owner_.with_capture_read_budget([&](auto &guard) { return capture.plan_head(captured.head, captured.generation, owner_.reclaim_check_, guard); }); !status) return status;
        if (!owner_.reclaim_check_.valid || !Owner::same_reclaim(owner_.reclaim_check_.reclaim, homes))
            return Status::fault("empty orphan final whole preparation differs");
        { smp::ScopedSpinLock guard(owner_.cache_.lock_); status = owner_.prepared(true); }
        if (!status) return status;
        workspace_->sealed_original_ = storage;
        seal_.owner = this; seal_.canonical_owner = &owner_; seal_.cache = &owner_.cache_;
        seal_.session = &owner_.cache_.session_; seal_.workspace = workspace_; seal_.storage = &storage;
        const auto cache = owner_.cache_.report();
        seal_.identity = cache.device_identity; seal_.registration = cache.registration_generation;
        seal_.session_serial = cache.session_serial; seal_.first_sector = cache.first_sector; seal_.sectors = cache.sectors;
        seal_.domain = owner_.publication_domain_; seal_.head = captured.head; seal_.generation = captured.generation;
        seal_.sequence = owner_.reclaim_journal_.sequence; seal_.next_head = homes.next_head;
        seal_.chain_members = captured.chain_members; seal_.uuid = capture.binding_.uuid; seal_.active = true;
        owner_.reclaim_prepared_ = owner_.reclaim_active_ = true;
        // No runtime Ticket, lifetime record, or claimant is fabricated.
        owner_.reclaim_io_ = capture.report_.original_io;
        if (owner_.reclaim_io_.reads > ~block::SessionCounter{0} - capture.report_.session_reads)
            return Status::overflow("empty orphan original read counter exhausted");
        owner_.reclaim_io_.reads += capture.report_.session_reads;
        owner_.reclaim_io_.completed_reads = owner_.reclaim_io_.reads;
        if (auto exact = owner_.reclaim_session_exact(); !exact) return exact;
        return guard();
    }
    [[nodiscard]] bool original_storage_exact() const
    {
        if (!workspace_) return false;
        const auto &a = workspace_->original_, &b = workspace_->sealed_original_;
        if (a.superblock != b.superblock || a.descriptors != b.descriptors || a.journal != b.journal ||
            a.inode_bitmaps != b.inode_bitmaps || a.block_bitmaps != b.block_bitmaps || a.inode_tables != b.inode_tables)
            return false;
        for (usize index = 0; index < a.bootstrap_reads.size(); ++index)
            if (a.bootstrap_reads[index].physical != b.bootstrap_reads[index].physical ||
                a.bootstrap_reads[index].bytes != b.bootstrap_reads[index].bytes) return false;
        for (usize index = 0; index < a.chain.size(); ++index)
            if (a.chain[index].number != b.chain[index].number ||
                a.chain[index].expected_generation != b.chain[index].expected_generation ||
                a.chain[index].table_block != b.chain[index].table_block || a.chain[index].table != b.chain[index].table)
                return false;
        for (usize index = 0; index < a.directories.size(); ++index)
            if (a.directories[index].inode != b.directories[index].inode ||
                a.directories[index].logical != b.directories[index].logical ||
                a.directories[index].physical != b.directories[index].physical || a.directories[index].bytes != b.directories[index].bytes)
                return false;
        return true;
    }
    [[nodiscard]] Status guard()
    {
        const auto &capture = owner_.reclaim_capture_; const auto &homes = owner_.reclaim_plan_.reclaim;
        const auto &journal = owner_.reclaim_journal_; const auto cache = owner_.cache_.report();
        if (!seal_.active || seal_.owner != this || seal_.canonical_owner != &owner_ || seal_.cache != &owner_.cache_ ||
            seal_.session != &owner_.cache_.session_ || seal_.workspace != workspace_ || !workspace_ ||
            workspace_->claimant_ != this || !original_storage_exact() || seal_.storage != &workspace_->original_ ||
            owner_.reclaim_storage_ != seal_.storage || owner_.reclaim_scratch_ != &workspace_->preparation_ ||
            owner_.reclaim_lifetime_ || owner_.reclaim_ticket_.valid() ||
            seal_.identity != cache.device_identity || seal_.registration != cache.registration_generation ||
            seal_.session_serial != cache.session_serial || seal_.first_sector != cache.first_sector || seal_.sectors != cache.sectors ||
            seal_.domain != owner_.publication_domain_ || seal_.head != capture.report_.head ||
            seal_.generation != capture.report_.generation || seal_.chain_members != capture.report_.chain_members ||
            seal_.head != homes.inode || seal_.generation != homes.old_generation || seal_.next_head != homes.next_head ||
            seal_.uuid != capture.binding_.uuid || seal_.sequence != journal.sequence ||
            capture.binding_.journal_sequence == ~u32{0} || capture.binding_.journal_sequence + 1 != journal.sequence ||
            !capture.report_.valid || !owner_.reclaim_plan_.valid || !journal.ready || !owner_.reclaim_prepared_ ||
            !owner_.reclaim_active_ || homes.count < 4 || homes.count > 5 || journal.count != homes.count ||
            owner_.participant_count_ != homes.count + 1 || owner_.blocks_[0] != capture.binding_.layout.journal_first_block ||
            owner_.sealed_[0] != journal.journal_head)
            return Status::fault("empty orphan private cold authority differs");
        for (usize index = 0; index < homes.count; ++index)
            if (owner_.blocks_[index + 1] != homes.targets[index] || journal.targets[index] != homes.targets[index] ||
                owner_.sealed_[index + 1] != homes.after[index])
                return Status::fault("empty orphan original canonical whole homes differ");
        smp::ScopedSpinLock guard(owner_.cache_.lock_);
        return owner_.reclaim_published_ ? owner_.authorized() : owner_.prepared(true);
    }
    [[nodiscard]] Status write(u32 physical, const Block &bytes)
    {
        if (auto status = guard(); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        auto &io = owner_.reclaim_io_;
        if (physical >= owner_.reclaim_capture_.binding_.layout.filesystem_blocks || io.writes == ~block::SessionCounter{0})
            return Status::overflow("empty orphan write bounds or counter exhausted");
        if (auto status = owner_.cache_.session_.write_blocks(u64{physical} * 2, std::as_bytes(std::span{bytes})); !status)
            return status;
        ++io.writes; ++io.completed_writes; io.pending_writes = true;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        return guard();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = guard(); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        auto &io = owner_.reclaim_io_;
        if (io.flushes == ~block::SessionCounter{0}) return Status::overflow("empty orphan flush counter exhausted");
        if (auto status = owner_.cache_.session_.flush(); !status) return status;
        ++io.flushes; ++io.completed_flushes; io.pending_writes = false;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        return guard();
    }
    template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Registry::Session>
    [[nodiscard]] Status compare(u32 physical, const Block &bytes, Hook &hook)
    {
        if (auto status = guard(); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        auto &io = owner_.reclaim_io_;
        if (io.pending_writes || io.reads == ~block::SessionCounter{0}) return Status::overflow("empty orphan unflushed readback or counter");
        auto authority = [this]() -> Status { return guard(); };
        if (auto status = guarded_ext4_direct_read(owner_.cache_.session_, io, authority,
            u64{physical} * 2, std::as_writable_bytes(std::span{owner_.reclaim_readback_}), hook); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        if (owner_.reclaim_readback_ != bytes) return Status::io_error("empty orphan durable whole readback differs");
        return guard();
    }
    [[nodiscard]] Status compare(u32 physical, const Block &bytes)
    { Ext4DirectReadHooks hook; return compare(physical, bytes, hook); }
    [[nodiscard]] Status compare_homes(usize after_count)
    {
        const auto &homes = owner_.reclaim_plan_.reclaim;
        for (usize index = 0; index < homes.count; ++index)
            if (auto status = compare(homes.targets[index], index < after_count ? homes.after[index] : homes.before[index]); !status)
                return status;
        return {};
    }
    [[nodiscard]] Status verify_device(bool after)
    {
        if (auto status = guard(); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        auto &disk = owner_.reclaim_disk_capture_;
        disk.~Ext4OrphanReclaimCapture<DeviceCapacity>();
        ::new (static_cast<void *>(&disk)) Ext4OrphanReclaimCapture<DeviceCapacity>;
        auto binding = owner_.reclaim_capture_.binding_;
        binding.empty_log = false; binding.journal_sequence = owner_.reclaim_journal_.sequence;
        auto &storage = workspace_->observed_; auto &scratch = workspace_->verification_;
        if (auto status = owner_.with_capture_read_budget([&](auto &guard) -> Status {
            return disk.capture(owner_.cache_.session_, binding, storage, scratch, guard);
        }); !status) return status;
        const auto &captured = disk.report(); auto &io = owner_.reclaim_io_;
        if (!Owner::exact_session(captured.original, io) || io.reads > ~block::SessionCounter{0} - captured.reads)
            return Status::fault("empty orphan actual journal original counters differ");
        io.reads += captured.reads; io.completed_reads += captured.reads;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        const auto &semantic = captured.semantic; const auto &homes = owner_.reclaim_plan_.reclaim;
        const auto &journal = scratch.canonical; const auto &original = owner_.reclaim_journal_;
        if (!captured.valid || !semantic.valid || !semantic.complete_global_before_proof ||
            (after ? !semantic.all_after : !semantic.all_before) || semantic.inode != seal_.head ||
            semantic.old_generation != seal_.generation || semantic.active_sequence != seal_.sequence ||
            semantic.parsed_next_sequence != seal_.sequence + 1 || semantic.cleanup_sequence != seal_.sequence + 2 ||
            !scratch.reclaim.valid || !Owner::same_reclaim(scratch.reclaim.reclaim, homes) ||
            !journal.ready || journal.sequence != original.sequence || journal.count != original.count ||
            journal.journal_head != original.journal_head || journal.descriptor != original.descriptor ||
            journal.commit != original.commit || journal.end != original.end)
            return Status::fault("empty orphan actual full cold proof differs from private seal");
        for (usize index = 0; index < homes.count; ++index)
            if (journal.targets[index] != original.targets[index] || journal.payloads[index] != original.payloads[index])
                return Status::fault("empty orphan actual journal payload differs");
        const auto &old = workspace_->original_;
        if (scratch.superblock != old.superblock || scratch.descriptors != old.descriptors ||
            scratch.inode_bitmaps != old.inode_bitmaps || scratch.block_bitmaps != old.block_bitmaps)
            return Status::fault("empty orphan reconstructed complete before controls differ");
        for (usize group = 0; group < 4; ++group)
            if (owner_.reclaim_capture_.initialized_[group] && scratch.tables[group] != old.inode_tables[group])
                return Status::fault("empty orphan reconstructed full before inode tables differ");
        if (disk.directory_count_ != owner_.reclaim_capture_.directory_count_)
            return Status::fault("empty orphan complete directory count differs");
        for (usize index = 0; index < disk.directory_count_; ++index)
        {
            const auto &a = old.directories[index], &b = storage.directories[index];
            if (a.inode != b.inode || a.logical != b.logical || a.physical != b.physical || a.bytes != b.bytes)
                return Status::fault("empty orphan complete before directory differs");
        }
        owner_.reclaim_empty_head_ = original.journal_head;
        const auto be32 = [&](usize at, u32 value) {
            for (usize byte = 0; byte < 4; ++byte) owner_.reclaim_empty_head_[at + byte] = u8(value >> (24 - 8 * byte));
        };
        be32(24, semantic.cleanup_sequence); be32(28, 0); be32(252, 0);
        be32(252, jbd2::detail::crc32c(~u32{0}, owner_.reclaim_empty_head_));
        { smp::ScopedSpinLock guard(lock_); ++report_.verified_publications; }
        return guard();
    }
    [[nodiscard]] Status publish()
    {
        if (auto status = guard(); !status) return status;
        // Complete final before proof immediately precedes the first FLUSH.
        if (auto status = owner_.with_capture_read_budget([&](auto &guard) { return owner_.reclaim_capture_.plan_head(seal_.head, seal_.generation, owner_.reclaim_check_, guard); }); !status) return status;
        if (!owner_.reclaim_check_.valid || !Owner::same_reclaim(owner_.reclaim_check_.reclaim, owner_.reclaim_plan_.reclaim))
            return Status::fault("empty orphan publication final whole before proof differs");
        const auto &capture = owner_.reclaim_capture_.report_;
        owner_.reclaim_io_ = capture.original_io;
        if (owner_.reclaim_io_.reads > ~block::SessionCounter{0} - capture.session_reads)
            return Status::overflow("empty orphan final capture counter exhausted");
        owner_.reclaim_io_.reads += capture.session_reads; owner_.reclaim_io_.completed_reads = owner_.reclaim_io_.reads;
        const auto &j = owner_.reclaim_journal_;
        const u32 first = u32(owner_.reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = flush(); !status) return status;
        if (auto status = write(first + 1, j.descriptor); !status) return status;
        for (usize index = 0; index < j.count; ++index)
            if (auto status = write(first + 2 + u32(index), j.payloads[index]); !status) return status;
        if (auto status = write(first + 3 + u32(j.count), j.end); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = write(first + 2 + u32(j.count), j.commit); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = write(first, j.journal_head); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = verify_device(false); !status) return status;
        Status status;
        {
            smp::ScopedSpinLock guard(owner_.cache_.lock_);
            status = owner_.prepared(true);
            if (status) { owner_.reclaim_published_ = true; owner_.activate(); }
        }
        return status;
    }
    [[nodiscard]] Status checkpoint()
    {
        if (auto status = verify_device(false); !status) return status;
        const auto &homes = owner_.reclaim_plan_.reclaim;
        if (auto status = write(homes.targets[0], homes.after[0]); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = compare_homes(1); !status) return status;
        for (usize index = 1; index + 1 < homes.count; ++index)
            if (auto status = write(homes.targets[index], homes.after[index]); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = compare_homes(homes.count - 1); !status) return status;
        if (auto status = write(homes.targets[homes.count - 1], homes.after[homes.count - 1]); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = compare_homes(homes.count); !status) return status;
        if (auto status = verify_device(true); !status) return status;
        const u32 journal = u32(owner_.reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = write(journal, owner_.reclaim_empty_head_); !status) return status;
        if (auto status = flush(); !status) return status;
        if (auto status = compare_homes(homes.count); !status) return status;
        if (auto status = compare(journal, owner_.reclaim_empty_head_); !status) return status;
        if (!homes.next_head)
        {
            TerminalView terminal(*this);
            if (auto status = workspace_->terminal_binding_.inspect(terminal, {0, owner_.cache_.session_.geometry().block_count}); !status)
                return status;
            const auto *actual = workspace_->terminal_binding_.info(); const auto &old = owner_.reclaim_capture_.binding_;
            if (!actual || !actual->empty_log || !actual->needs_recovery || actual->uuid != seal_.uuid ||
                actual->journal_sequence != seal_.sequence + 2 || actual->layout.journal_first_block != old.layout.journal_first_block ||
                actual->layout.journal_blocks != old.layout.journal_blocks || actual->metadata_count != old.metadata_count)
                return Status::fault("empty orphan terminal ordinary binding differs");
            for (usize index = 0; index < old.metadata_count; ++index)
                if (actual->metadata[index].first != old.metadata[index].first || actual->metadata[index].count != old.metadata[index].count)
                    return Status::fault("empty orphan terminal metadata placement differs");
        }
        Status status;
        {
            smp::ScopedSpinLock guard(owner_.cache_.lock_);
            status = owner_.authorized();
            if (status)
            {
                owner_.sealed_[0] = owner_.reclaim_empty_head_;
                owner_.publish(owner_.sealed_); owner_.authority_.active = false;
            }
        }
        if (!status) return status;
        if (auto retired = owner_.retire(); !retired) return retired;
        if (auto exact = owner_.reclaim_session_exact(); !exact) return exact;
        owner_.reclaim_prepared_ = false; owner_.reclaim_completed_ = true; seal_.active = false;
        return owner_.finish(Ext4OwnedPhase::checkpointed, {});
    }
    Owner owner_{};
    Ext4EmptyOrphanRecoveryWorkspace *workspace_{};
    ColdSeal seal_{};
    std::array<std::array<u32, 2>, 8> initial_members_{};
    std::array<u8, 16> initial_uuid_{};
    Block expected_empty_head_{};
    usize expected_directory_count_{};
    Ext4EmptyOrphanRecoveryReport report_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::fs
