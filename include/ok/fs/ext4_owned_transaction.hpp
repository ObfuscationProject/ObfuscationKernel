#pragma once

#include "ok/fs/ext4_mounted_typed_cursor.hpp"
#include "ok/fs/ext4_mounted_common_opening_budget.hpp"
#include "ok/fs/ext4_direct_read_scope.hpp"

#include "ok/block/guarded_reads.hpp"
#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_rename_transaction.hpp"
#include "ok/fs/ext4_create_transaction.hpp"
#include "ok/fs/ext4_directory_initialization_budget.hpp"
#include "ok/fs/ext4_first_data_transaction.hpp"
#include "ok/fs/ext4_next_data_transaction.hpp"
#include "ok/fs/ext4_namespace_transaction.hpp"
#include "ok/fs/ext4_namespace_completion.hpp"
#include "ok/fs/ext4_regular_move_transaction.hpp"
#include "ok/fs/ext4_regular_move_completion.hpp"
#include "ok/fs/metadata_buffers.hpp"
#include "ok/fs/ext4_orphan_chain_capture.hpp"
#include "ok/fs/ext4_orphan_reclaim_journal.hpp"
#include "ok/fs/ext4_inode_lifetime.hpp"
#include "ok/fs/ext4_orphan_reclaim_capture.hpp"
#include "ok/fs/ext4_mounted_startup_workspace.hpp"
#include "ok/fs/ext4_reclaim_route_budget.hpp"
#include "ok/fs/ext4_zero_truncate_transaction.hpp"
#include "ok/fs/ext4_allocated_empty_data_transaction.hpp"
#include <new>

namespace ok::fs
{
enum class Ext4OwnedPhase : u8
{
    unused, opening, open, preparing, prepared, publishing, durable,
    checkpointing, checkpointed, discarding, discarded, rejected, closing, closed, failed,
    orphan_cleanup_pending, orphan_cleanup_capturing, orphan_reclaim_prepared,
    orphan_reclaim_publishing, orphan_reclaim_durable, orphan_reclaim_checkpointing,
};
struct Ext4OwnedReport
{
    Ext4OwnedPhase phase{Ext4OwnedPhase::unused};
    Status status{};
    Ext4TransactionReport writer{};
    Ext4CompletionReport completion{};
    Ext4RegularMoveCompletionReport regular_move_completion{};
    bool regular_move{}, cold_regular_move{}, cold_namespace{}, orphan_cleanup_pending{}, orphan_reclaim_prepared{}, orphan_reclaimed{};
};

// One ordinary-inode transaction and checkpoint in one private canonical owner.
// Registry, device and owner storage outlive every pin and admitted operation.
// Failure retains quarantine until transport lifetime end or external fencing.
template <usize DeviceCapacity = 4, usize BufferCapacity = 3>
class Ext4OwnedTransaction final
{
    template <usize> friend class Ext4MountedMetadata;
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
    static_assert(BufferCapacity >= 3);
    using Cache = MetadataBuffers<BufferCapacity, 1024, DeviceCapacity>;
    using Block = std::array<u8, 1024>;
    static constexpr usize home_capacity = 8;
    static constexpr usize participant_capacity = home_capacity + 1;
    using Completion = Ext4RecoveryCompletion<home_capacity, 2>;
    struct Authority final
    {
        Authority() = default;
        Authority(const Authority &) = delete;
        Authority &operator=(const Authority &) = delete;
        const Ext4OwnedTransaction *owner{};
        const Cache *cache{};
        const typename Cache::Registry::Session *session{};
        const std::array<Block, participant_capacity> *images{};
        u64 identity{}, registration{}, session_serial{}, transaction{}, domain{}, first_sector{}, sectors{};
        std::array<usize, participant_capacity> slots{};
        std::array<u64, participant_capacity> generations{};
        u32 sequence{};
        bool active{};
    };

  public:
    using Registry = typename Cache::Registry;
    using Pin = typename Cache::Pin;
    explicit Ext4OwnedTransaction(bool mounted_dirty = false)
        : writer_(mounted_dirty), completion_(mounted_dirty, mounted_dirty), mounted_dirty_(mounted_dirty) {}
    Ext4OwnedTransaction(const Ext4OwnedTransaction &) = delete;
    Ext4OwnedTransaction &operator=(const Ext4OwnedTransaction &) = delete;
    Ext4OwnedTransaction(Ext4OwnedTransaction &&) = delete;
    Ext4OwnedTransaction &operator=(Ext4OwnedTransaction &&) = delete;
    ~Ext4OwnedTransaction()
    {
        // Quarantine before internal handles destruct, even after a durable flush.
        if (cache_.report().state == MetadataOwnerState::ready)
            static_cast<void>(cache_.abort(Status::interrupted("owned ext4 destroyed without close")));
    }

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
                              block::SessionBounds bounds)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4OwnedPhase::unused) return Status::already_exists("owned ext4 is one-shot");
            report_.phase = Ext4OwnedPhase::opening;
        }
        auto status = cache_.open(registry, key, bounds, block::SessionAccess::writable);
        if (!status) return finish(Ext4OwnedPhase::closed, status);
        if (cache_.session_.geometry().block_size != 512)
        {
            status = cache_.close();
            if (!status) return fail(status);
            return finish(Ext4OwnedPhase::closed, Status::unsupported("owned ext4 requires 512-byte sectors"));
        }
        // Session offsets are partition-relative; only BlockSessions adds origin.
        bounds_ = {0, bounds.count};
        return finish(Ext4OwnedPhase::open, {});
    }

    [[nodiscard]] Status pin(u64 block, Pin &pin)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = stable(); !status) return status;
            if (admissions_ == ~usize{0}) return Status::overflow("owned ext4 reader admission exhausted");
            ++admissions_;
        }
        const auto status = cache_.pin(block, pin);
        {
            smp::ScopedSpinLock guard(lock_);
            --admissions_;
            if (cache_.report().state == MetadataOwnerState::failed)
            { report_.phase = Ext4OwnedPhase::failed; report_.status = status; }
        }
        return status;
    }

    [[nodiscard]] Status prepare(Ext4InodeMetadataUpdate update)
    {
        return prepare_for<true>(update);
    }

    [[nodiscard]] Status prepare_ownership(Ext4InodeMetadataUpdate update)
    {
        return prepare_for<true, true>(update);
    }

    [[nodiscard]] Status prepare_mode(Ext4InodeMetadataUpdate update)
    {
        return prepare_for<true, false, true>(update);
    }

    [[nodiscard]] Status prepare_ordered_data(Ext4InodeMetadataUpdate update, u32 data_block,
                                               const Block &data_image)
    {
        if (!update.ordered_data || BufferCapacity < 4 || !data_block)
            return Status::invalid_argument("ordered-data owner requires four buffers and a block");
        return prepare_for<false>(update, data_block, &data_image);
    }

    // The mounted caller owns namespace/credential admission. This stages the
    // journal head, inode table(s) and directory as one canonical transaction.
    [[nodiscard]] Status prepare_rename(Ext4RenameUpdate update)
    {
        if (!mounted_dirty_ || BufferCapacity < 4)
            return Status::unsupported("owned rename requires mounted-dirty four-buffer policy");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        rename_active_ = true;
        if (auto status = rename_writer_.prepare(cache_.session_, bounds_, update); !status)
            return reject(status);
        std::array<u64, 4> blocks{};
        std::array<Block, 4> images{};
        if (auto status = rename_writer_.logical_images(blocks, images, participant_count_); !status)
            return fail(status);
        for (usize index = 0; index < participant_count_; ++index)
        { blocks_[index] = blocks[index]; sealed_[index] = images[index]; }
        completion_.~Completion();
        ::new (static_cast<void *>(&completion_)) Completion(
            false, true, Ext4ReplayPolicy::directory_rename);
        return stage_prepared();
    }

    #include "ok/fs/ext4_owned_regular_move.inc"

    [[nodiscard]] Status prepare_create(Ext4CreateUpdate update)
    {
        const bool directory_create = update.attributes.type == NodeType::directory;
        if (!mounted_dirty_ || BufferCapacity < (directory_create ? 9U : 7U))
            return Status::unsupported("owned create requires seven regular or nine directory buffers");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        if (directory_create)
            if (auto status = reserve_directory_io(); !status) return reject(status);
        create_active_ = true;
        const auto preparation = create_writer_.prepare(cache_.session_, bounds_, update);
        if (!preparation) return reject(preparation);
        if (auto status = create_writer_.logical_images(blocks_, sealed_, participant_count_); !status)
            return fail(status);
        completion_.~Completion();
        ::new (static_cast<void *>(&completion_)) Completion(false, true, directory_create ?
            Ext4ReplayPolicy::bounded_directory_create : Ext4ReplayPolicy::directory_create);
        return stage_prepared();
    }
    [[nodiscard]] Status prepare_first_data(Ext4FirstDataUpdate update, u32 offset, const Block &image)
    {
        if (!mounted_dirty_ || BufferCapacity < 6)
            return Status::unsupported("owned first data requires mounted-dirty six-buffer policy");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        first_data_active_ = true;
        if (auto status = first_data_writer_.prepare(cache_.session_, bounds_, update, offset, image); !status)
            return reject(status);
        if (auto status = first_data_writer_.logical_images(blocks_, sealed_, participant_count_); !status)
            return fail(status);
        has_data_ = true; data_block_ = u32(blocks_[5]); data_image_ = sealed_[5];
        completion_.~Completion();
        ::new (static_cast<void *>(&completion_)) Completion(false, true, Ext4ReplayPolicy::first_data_allocation);
        return stage_prepared();
    }
    [[nodiscard]] Status prepare_next_data(Ext4NextDataUpdate update, u32 offset, const Block &image)
    {
        if (!mounted_dirty_ || BufferCapacity < 6)
            return Status::unsupported("owned next data requires mounted-dirty six-buffer policy");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        next_data_active_ = true;
        if (auto status = next_data_writer_.prepare(cache_.session_, bounds_, update, offset, image); !status)
            return reject(status);
        if (auto status = next_data_writer_.logical_images(blocks_, sealed_, participant_count_); !status)
            return fail(status);
        has_data_ = true; data_block_ = u32(blocks_[5]); data_image_ = sealed_[5];
        completion_.~Completion();
        ::new (static_cast<void *>(&completion_)) Completion(false, true, Ext4ReplayPolicy::next_data_allocation);
        return stage_prepared();
    }
    // Private owner-only namespace admission. The mounted provider supplies
    // its exclusive operation epoch and checked runtime inode identities.
    [[nodiscard]] Status prepare_namespace(Ext4NamespaceUpdate update)
    {
        if (!mounted_dirty_ || BufferCapacity < 6)
            return Status::unsupported("owned namespace requires mounted-dirty six-buffer policy");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        namespace_active_ = true;
        if (auto status = namespace_writer_.prepare(cache_.session_, bounds_, update); !status)
            return reject(status);
        if (auto status = namespace_writer_.logical_images(blocks_, sealed_, participant_count_); !status)
            return fail(status);
        return stage_prepared();
    }
    [[nodiscard]] u32 created_inode() const { return create_active_ ? create_writer_.report().inode : 0; }

    // A fresh owner admits only the committed private-witness namespace family.
    // This grants checkpoint authority, never descriptor lifetime or reclaim.
    [[nodiscard]] Status prepare_namespace_recovery()
    {
        if (BufferCapacity < 6)
            return Status::unsupported("namespace recovery requires six canonical buffers");
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::preparing;
        }
        namespace_active_ = namespace_recovery_active_ = true;
        if (auto status = namespace_completion_.prepare(cache_.session_, bounds_); !status)
            return fail(status);
        if (auto status = namespace_completion_.logical_images(blocks_, sealed_, participant_count_); !status)
            return fail(status);
        return stage_prepared(true);
    }
    [[nodiscard]] Status recover_namespace()
    {
        if (auto status = prepare_namespace_recovery(); !status) return status;
        return checkpoint();
    }

  private:
    // Each plain capture/comparison receives a fresh synchronous read scope.
    // Opening reservation conservatively checks whole-route counter headroom,
    // but this helper never refines the budget or grants WRITE/FLUSH authority.
    using CaptureReadBudget = Ext4ReclaimRouteBudget<typename Registry::Session>;
    template <typename Operation>
        requires requires(Operation &operation, CaptureReadBudget &guard)
        { { operation(guard) } -> std::same_as<Status>; }
    [[nodiscard]] Status with_capture_read_budget(Operation &&operation)
    {
        CaptureReadBudget guard;
        auto &session = cache_.session_;
        if (auto status = guard.reserve_opening(session); !status) return status;
        if (auto status = operation(guard); !status) return status;
        return guard.close(session);
    }

    // Preparation only. Mounted lookup-to-retain retirement and writable
    // publication/checkpoint remain absent; public commit cannot enter this
    // prepared phase. No block or inode is freed by this method.
    [[nodiscard]] Status prepare_orphan_reclaim(Ext4InodeLifetimeTable<128, 512> &lifetime,
        typename Ext4InodeLifetimeTable<128, 512>::Ticket ticket,
        Ext4InodeLifetimeKey expected, Ext4OrphanChainCaptureStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch, u32 seconds, u32 nanoseconds)
    {
        if (!mounted_dirty_ || BufferCapacity < 6)
            return Status::unsupported("orphan owner preparation requires six writable credits");
        if (!expected.mount || !expected.inode || expected.session != cache_.report().session_serial)
            return Status::bad_descriptor("orphan owner expected lifetime identity");
        if (auto status = lifetime.validate_dispatched(ticket, expected); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4OwnedPhase::orphan_cleanup_pending) return phase_error();
            const auto cache = cache_.report();
            if (!namespace_active_ || !namespace_completion_.report().complete() ||
                !namespace_completion_.orphan_cleanup_pending() || !report_.orphan_cleanup_pending ||
                !namespace_completion_.admission_.result() || authority_.active || admissions_ ||
                cache.state != MetadataOwnerState::ready || cache.in_flight || cache.read_pins ||
                cache.transaction_active)
                return Status::busy("orphan owner preparation has retained or incomplete work");
            if (namespace_completion_.admission_.result()->removed_inode != expected.inode)
                return Status::bad_descriptor("orphan ticket differs from completed namespace victim");
            report_.phase = Ext4OwnedPhase::orphan_cleanup_capturing;
        }
        reclaim_storage_ = &storage; reclaim_scratch_ = &scratch;
        reclaim_lifetime_ = &lifetime; reclaim_ticket_ = ticket; reclaim_key_ = expected;
        if (auto status = with_capture_read_budget([&](auto &guard) { return reclaim_capture_.capture(cache_.session_, storage, scratch, guard); }); !status) return fail(status);
        const auto captured = reclaim_capture_.report();
        if (captured.head != expected.inode || captured.generation != expected.generation ||
            captured.identity != cache_.report().device_identity ||
            captured.registration_generation != cache_.report().registration_generation ||
            captured.session_serial != expected.session)
            return fail(Status::fault("orphan owner captured identity differs"));
        if (auto status = with_capture_read_budget([&](auto &guard) { return reclaim_capture_.plan_head(expected.inode, expected.generation, reclaim_plan_, guard); }); !status)
            return fail(status);
        const auto &homes = reclaim_plan_.reclaim;
        if (!reclaim_plan_.valid || homes.count < 4 || homes.count > 5 ||
            homes.count + 1 > BufferCapacity || homes.released_blocks > homes.released_mapping.size())
            return fail(Status::fault("orphan owner complete plan profile"));
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(reclaim_capture_.binding_, homes,
                storage.journal, seconds, nanoseconds, reclaim_format_, reclaim_journal_); !status)
            return fail(status);
        // Bind this exact earlier namespace checkpoint, even when an old
        // canonical cache participant has since been evicted.
        for (usize index = 0; index < participant_count_; ++index)
        {
            const auto *observed = captured_orphan_block(blocks_[index]);
            if (!observed || *observed != sealed_[index])
                return fail(Status::fault("orphan source differs from original namespace checkpoint"));
        }
        const u64 journal = reclaim_capture_.binding_.layout.journal_first_block;
        std::array<u64, 16> released{};
        for (usize index = 0; index < homes.released_blocks; ++index)
        {
            const u64 physical = homes.released_mapping[index]; released[index] = physical;
            const auto &binding = reclaim_capture_.binding_;
            if (physical < 3 || physical >= binding.layout.filesystem_blocks ||
                (physical >= journal && physical - journal < binding.layout.journal_blocks))
                return fail(Status::fault("orphan released mapping aliases control blocks"));
            for (const auto &range : binding.structural_metadata().metadata)
                if (physical >= range.first && physical - range.first < range.count)
                    return fail(Status::fault("orphan released mapping aliases metadata"));
            for (usize home = 0; home < homes.count; ++home)
                if (physical == homes.targets[home]) return fail(Status::fault("orphan released mapping aliases participant"));
        }
        if (auto status = lifetime.validate_dispatched(ticket, expected); !status) return fail(status);
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = reconcile_orphan_cache();
        }
        if (!status) return fail(status);
        if (auto retired = cache_.retire_clean_blocks(std::span<const u64>{released}.first(homes.released_blocks)); !retired)
            return fail(retired.status());
        // Recycle drained handles only. Cache slot generations, transaction
        // serial and the original session never restart.
        auto reset = []<typename T>(T &object) { object.~T(); ::new (static_cast<void *>(&object)) T; };
        reset(transaction_); for (auto &pin : pins_) reset(pin); reset(authority_);
        blocks_ = {}; sealed_ = {}; participant_count_ = homes.count + 1;
        blocks_[0] = journal; sealed_[0] = reclaim_journal_.journal_head;
        for (usize index = 0; index < homes.count; ++index)
        { blocks_[index + 1] = homes.targets[index]; sealed_[index + 1] = homes.after[index]; }
        for (usize index = 0; index < participant_count_; ++index)
        {
            if (status = reclaim_capture_.stable(); !status) return fail(status);
            const auto before = cache_.session_.report(); if (!before) return fail(before.status());
            if (status = cache_.pin(blocks_[index], pins_[index]); !status) return fail(status);
            const auto after = cache_.session_.report(); if (!after) return fail(after.status());
            // A canonical pin may perform exactly one known cache-miss read.
            if (after.value().reads < before.value().reads || after.value().reads - before.value().reads > 1 ||
                after.value().completed_reads != after.value().reads ||
                reclaim_capture_.report_.session_reads > ~block::SessionCounter{0} - (after.value().reads - before.value().reads))
                return fail(Status::fault("orphan canonical pin read accounting"));
            reclaim_capture_.report_.session_reads += after.value().reads - before.value().reads;
            if (status = reclaim_capture_.stable(); !status) return fail(status);
            const Block &old = index ? homes.before[index - 1] : storage.journal;
            {
                smp::ScopedSpinLock guard(cache_.lock_);
                const auto *entry = cache_.find(pins_[index]);
                if (!entry || entry->report.block != blocks_[index] ||
                    entry->report.state != MetadataBufferState::ready || entry->report.staged_dirty)
                    status = Status::fault("orphan canonical before identity");
                else for (usize byte = 0; byte < 1024; ++byte)
                    if (entry->canonical[byte] != std::byte(old[byte]))
                    { status = Status::fault("orphan canonical before bytes differ"); break; }
            }
            if (!status) return fail(status);
        }
        std::array<Pin *, participant_capacity> participants{};
        for (usize index = 0; index < participant_count_; ++index) participants[index] = &pins_[index];
        if (status = cache_.begin(std::span<Pin *const>{participants}.first(participant_count_), transaction_); !status)
            return fail(status);
        for (usize index = 0; index < participant_count_; ++index)
            if (status = transaction_.edit(blocks_[index], 0, std::as_bytes(std::span{sealed_[index]})); !status)
                return fail(status);
        if (status = with_capture_read_budget([&](auto &guard) { return reclaim_capture_.plan_head(expected.inode, expected.generation, reclaim_check_, guard); }); !status)
            return fail(status);
        const auto &checked = reclaim_check_.reclaim;
        if (!reclaim_check_.valid || checked.count != homes.count || checked.inode != homes.inode ||
            checked.old_generation != homes.old_generation || checked.retained_generation != homes.retained_generation ||
            checked.next_head != homes.next_head || checked.released_blocks != homes.released_blocks ||
            checked.released_mapping != homes.released_mapping)
            return fail(Status::fault("orphan final checked plan identity differs"));
        for (usize index = 0; index < homes.count; ++index)
            if (checked.targets[index] != homes.targets[index] || checked.before[index] != homes.before[index] ||
                checked.after[index] != homes.after[index])
                return fail(Status::fault("orphan final checked whole homes differ"));
        if (status = lifetime.validate_dispatched(ticket, expected); !status) return fail(status);
        { smp::ScopedSpinLock guard(cache_.lock_); status = prepared(true); }
        if (!status) return fail(status);
        reclaim_prepared_ = true;
        return finish(Ext4OwnedPhase::orphan_reclaim_prepared, {});
    }
    // Same-owner writer only. No public supplied image/report can acquire this
    // authority. The mount must exclude every other session/storage user.
    [[nodiscard]] Status publish_orphan_reclaim(Ext4OrphanReclaimCaptureStorage &storage,
                                               Ext4OrphanReclaimColdScratch &scratch)
    {
        if (auto status = reclaim_storage_disjoint(storage, scratch); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4OwnedPhase::orphan_reclaim_prepared) return phase_error();
            if (admissions_) return Status::busy("orphan publication has reader admissions");
            report_.phase = Ext4OwnedPhase::orphan_reclaim_publishing;
        }
        if (!reclaim_lifetime_ || !reclaim_prepared_)
            return fail(Status::fault("orphan publication has no original preparation"));
        if (auto status = reclaim_lifetime_->claim_publication(reclaim_ticket_, reclaim_key_, this); !status)
            return fail(status);
        reclaim_active_ = true;
        if (auto status = reclaim_guard(); !status) return fail(status);
        if (auto status = with_capture_read_budget([&](auto &guard) { return reclaim_capture_.plan_head(reclaim_key_.inode, reclaim_key_.generation, reclaim_check_, guard); }); !status)
            return fail(status);
        if (!reclaim_check_.valid || !same_reclaim(reclaim_check_.reclaim, reclaim_plan_.reclaim))
            return fail(Status::fault("orphan publication original whole plan differs"));
        if (auto status = reclaim_guard(); !status) return fail(status);
        // Account only the reads performed by the original capture, retaining
        // every completed I/O counter from the earlier namespace checkpoint.
        reclaim_io_ = reclaim_capture_.report_.original_io;
        if (reclaim_io_.reads > ~block::SessionCounter{0} - reclaim_capture_.report_.session_reads)
            return fail(Status::overflow("orphan original read counter exhausted"));
        reclaim_io_.reads += reclaim_capture_.report_.session_reads;
        reclaim_io_.completed_reads = reclaim_io_.reads;
        if (auto status = reclaim_session_exact(); !status) return fail(status);
        const u32 journal = u32(reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_write(journal + 1, reclaim_journal_.descriptor); !status) return fail(status);
        for (usize index = 0; index < reclaim_journal_.count; ++index)
            if (auto status = reclaim_write(journal + 2 + u32(index), reclaim_journal_.payloads[index]); !status)
                return fail(status);
        if (auto status = reclaim_write(journal + 3 + u32(reclaim_journal_.count), reclaim_journal_.end); !status)
            return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_write(journal + 2 + u32(reclaim_journal_.count), reclaim_journal_.commit); !status)
            return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_write(journal, reclaim_journal_.journal_head); !status) return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = verify_reclaim_device(storage, scratch, false); !status) return fail(status);
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = prepared(true);
            if (status) { reclaim_published_ = true; activate(); }
        }
        if (!status) return fail(status);
        return finish(Ext4OwnedPhase::orphan_reclaim_durable, {});
    }

    [[nodiscard]] Status checkpoint_orphan_reclaim(Ext4OrphanReclaimCaptureStorage &storage,
                                                  Ext4OrphanReclaimColdScratch &scratch)
    {
        if (auto status = reclaim_storage_disjoint(storage, scratch); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4OwnedPhase::orphan_reclaim_durable) return phase_error();
            if (admissions_) return Status::busy("orphan checkpoint has reader admissions");
            report_.phase = Ext4OwnedPhase::orphan_reclaim_checkpointing;
        }
        if (auto status = reclaim_guard(); !status) return fail(status);
        if (auto status = reclaim_session_exact(); !status) return fail(status);
        // A fresh capture validates actual records and all original before
        // bytes before any home write. Captures are never weakened or rebased.
        if (auto status = verify_reclaim_device(storage, scratch, false); !status) return fail(status);
        const auto &homes = reclaim_plan_.reclaim;
        if (auto status = reclaim_write(homes.targets[0], homes.after[0]); !status) return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_check_homes(1); !status) return fail(status);
        for (usize index = 1; index + 1 < homes.count; ++index)
            if (auto status = reclaim_write(homes.targets[index], homes.after[index]); !status) return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_check_homes(homes.count - 1); !status) return fail(status);
        if (auto status = reclaim_write(homes.targets[homes.count - 1], homes.after[homes.count - 1]); !status)
            return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        if (auto status = reclaim_check_homes(homes.count); !status) return fail(status);
        // Reconstruct the complete declared-before history from the all-after
        // device with the still-active original journal, then check the seal.
        if (auto status = verify_reclaim_device(storage, scratch, true); !status) return fail(status);
        const u32 journal = u32(reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = reclaim_write(journal, reclaim_empty_head_); !status) return fail(status);
        if (auto status = reclaim_flush(); !status) return fail(status);
        for (usize index = 0; index < homes.count; ++index)
            if (auto status = reclaim_compare(homes.targets[index], homes.after[index]); !status) return fail(status);
        if (auto status = reclaim_compare(journal, reclaim_empty_head_); !status) return fail(status);
        if (auto status = reclaim_guard(); !status) return fail(status);
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = authorized();
            if (status)
            {
                sealed_[0] = reclaim_empty_head_;
                publish(sealed_); authority_.active = false;
            }
        }
        if (!status) return fail(status);
        if (auto status = retire(); !status) return fail(status);
        if (auto status = reclaim_session_exact(); !status) return fail(status);
        if (auto status = reclaim_lifetime_->complete_publication(reclaim_ticket_, reclaim_key_, this); !status)
            return fail(status);
        reclaim_prepared_ = false; reclaim_completed_ = true;
        return finish(Ext4OwnedPhase::checkpointed, {});
    }

    [[nodiscard]] static bool same_reclaim(const Ext4OrphanReclaimPlan &left,
                                         const Ext4OrphanReclaimPlan &right)
    {
        if (left.count < 4 || left.count > 5 || right.count != left.count ||
            left.inode != right.inode || left.old_generation != right.old_generation ||
            left.retained_generation != right.retained_generation || left.next_head != right.next_head ||
            left.released_blocks != right.released_blocks || left.released_mapping != right.released_mapping)
            return false;
        for (usize index = 0; index < left.count; ++index)
            if (left.targets[index] != right.targets[index] || left.before[index] != right.before[index] ||
                left.after[index] != right.after[index]) return false;
        return true;
    }
    [[nodiscard]] Status reclaim_guard()
    {
        if (!reclaim_active_ || !reclaim_lifetime_ || !reclaim_storage_ || !reclaim_scratch_ ||
            !reclaim_prepared_ || !reclaim_plan_.valid || !reclaim_journal_.ready ||
            reclaim_key_.session != cache_.report().session_serial ||
            reclaim_key_.inode != reclaim_plan_.reclaim.inode ||
            reclaim_key_.generation != reclaim_plan_.reclaim.old_generation ||
            reclaim_journal_.count < 4 || reclaim_journal_.count > 5 ||
            reclaim_plan_.reclaim.released_blocks > reclaim_plan_.reclaim.released_mapping.size() ||
            reclaim_journal_.count != reclaim_plan_.reclaim.count ||
            participant_count_ != reclaim_journal_.count + 1 ||
            blocks_[0] != reclaim_capture_.binding_.layout.journal_first_block ||
            sealed_[0] != reclaim_journal_.journal_head)
            return Status::fault("orphan original publication identity differs");
        for (usize index = 0; index < reclaim_journal_.count; ++index)
            if (blocks_[index + 1] != reclaim_plan_.reclaim.targets[index] ||
                reclaim_journal_.targets[index] != blocks_[index + 1] ||
                sealed_[index + 1] != reclaim_plan_.reclaim.after[index])
                return Status::fault("orphan original publication whole homes differ");
        if (auto status = reclaim_lifetime_->validate_publication(reclaim_ticket_, reclaim_key_, this); !status)
            return status;
        smp::ScopedSpinLock guard(cache_.lock_);
        return reclaim_published_ ? authorized() : prepared(true);
    }
    [[nodiscard]] static bool exact_session(const block::SessionReport &left, const block::SessionReport &right)
    {
        return left.state == right.state && left.first_error == right.first_error &&
            left.identity == right.identity && left.registration_generation == right.registration_generation &&
            left.session_serial == right.session_serial && left.reads == right.reads && left.writes == right.writes &&
            left.flushes == right.flushes && left.completed_reads == right.completed_reads &&
            left.completed_writes == right.completed_writes && left.completed_flushes == right.completed_flushes &&
            left.in_flight == right.in_flight && left.pending_writes == right.pending_writes;
    }
    [[nodiscard]] Status reclaim_session_exact() const
    {
        const auto current = cache_.session_.report();
        if (!current) return current.status();
        if (reclaim_io_.state != block::DeviceState::owned || reclaim_io_.first_error != StatusCode::ok ||
            reclaim_io_.in_flight || !exact_session(current.value(), reclaim_io_))
            return Status::io_error("orphan original session or exact counters differ");
        return {};
    }
    [[nodiscard]] Status reclaim_write(u32 physical, const Block &bytes)
    {
        if (auto status = reclaim_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (physical >= reclaim_capture_.binding_.layout.filesystem_blocks || reclaim_io_.writes == ~block::SessionCounter{0})
            return Status::overflow("orphan write block or counter exhausted");
        if (auto status = cache_.session_.write_blocks(u64{physical} * 2, std::as_bytes(std::span{bytes})); !status)
            return status;
        ++reclaim_io_.writes; ++reclaim_io_.completed_writes; reclaim_io_.pending_writes = true;
        if (auto status = reclaim_session_exact(); !status) return status;
        return reclaim_guard();
    }
    [[nodiscard]] Status reclaim_flush()
    {
        if (auto status = reclaim_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (reclaim_io_.flushes == ~block::SessionCounter{0}) return Status::overflow("orphan flush counter exhausted");
        if (auto status = cache_.session_.flush(); !status) return status;
        ++reclaim_io_.flushes; ++reclaim_io_.completed_flushes; reclaim_io_.pending_writes = false;
        if (auto status = reclaim_session_exact(); !status) return status;
        return reclaim_guard();
    }
    template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Registry::Session>
    [[nodiscard]] Status reclaim_compare(u32 physical, const Block &bytes, Hook &hook)
    {
        if (auto status = reclaim_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (reclaim_io_.pending_writes || reclaim_io_.reads == ~block::SessionCounter{0})
            return Status::overflow("orphan readback requires flushed counter capacity");
        auto authority = [this]() -> Status { return reclaim_guard(); };
        if (auto status = guarded_ext4_direct_read(cache_.session_, reclaim_io_, authority,
            u64{physical} * 2, std::as_writable_bytes(std::span{reclaim_readback_}), hook); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (reclaim_readback_ != bytes) return Status::io_error("orphan durable readback whole block differs");
        return reclaim_guard();
    }
    [[nodiscard]] Status reclaim_compare(u32 physical, const Block &bytes)
    { Ext4DirectReadHooks hook; return reclaim_compare(physical, bytes, hook); }
    [[nodiscard]] Status reclaim_check_homes(usize after_count)
    {
        const auto &homes = reclaim_plan_.reclaim;
        if (after_count > homes.count) return Status::invalid_argument("orphan checkpoint home mask");
        for (usize index = 0; index < homes.count; ++index)
            if (auto status = reclaim_compare(homes.targets[index], index < after_count ? homes.after[index] : homes.before[index]); !status)
                return status;
        return {};
    }
    [[nodiscard]] Status reclaim_storage_disjoint(const Ext4OrphanReclaimCaptureStorage &storage,
                                                 const Ext4OrphanReclaimColdScratch &scratch) const
    {
        if (!reclaim_storage_ || !reclaim_scratch_)
            return Status::not_initialized("orphan preparation storage absent");
        struct Range { const void *pointer; usize size; };
        const std::array<Range, 5> ranges{{{&storage, sizeof(storage)}, {&scratch, sizeof(scratch)},
            {reclaim_storage_, sizeof(*reclaim_storage_)}, {reclaim_scratch_, sizeof(*reclaim_scratch_)},
            {this, sizeof(*this)}}};
        for (usize left = 0; left < ranges.size(); ++left)
            for (usize right = left + 1; right < ranges.size(); ++right)
            {
                const auto a = reinterpret_cast<usize>(ranges[left].pointer), b = reinterpret_cast<usize>(ranges[right].pointer);
                if ((a <= b ? b - a < ranges[left].size : a - b < ranges[right].size))
                    return Status::invalid_argument("orphan publication storage overlaps");
            }
        return {};
    }
    [[nodiscard]] Status verify_reclaim_device(Ext4OrphanReclaimCaptureStorage &storage,
                                             Ext4OrphanReclaimColdScratch &scratch, bool after)
    {
        if (auto status = reclaim_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        // All prior requests have returned. Reset only this borrowed one-shot
        // verifier, never the cache, session, transaction or lifetime seal.
        reclaim_disk_capture_.~Ext4OrphanReclaimCapture<DeviceCapacity>();
        ::new (static_cast<void *>(&reclaim_disk_capture_)) Ext4OrphanReclaimCapture<DeviceCapacity>;
        auto binding = reclaim_capture_.binding_;
        binding.empty_log = false; binding.journal_sequence = reclaim_journal_.sequence;
        if (auto status = with_capture_read_budget([&](auto &guard) -> Status {
            return reclaim_disk_capture_.capture(cache_.session_, binding, storage, scratch, guard);
        }); !status) return status;
        const auto &capture = reclaim_disk_capture_.report();
        if (!exact_session(capture.original, reclaim_io_) || reclaim_io_.reads > ~block::SessionCounter{0} - capture.reads)
            return Status::fault("orphan published capture initial counters differ");
        reclaim_io_.reads += capture.reads; reclaim_io_.completed_reads += capture.reads;
        if (auto status = reclaim_session_exact(); !status) return status;
        const auto &semantic = capture.semantic; const auto &homes = reclaim_plan_.reclaim;
        const auto &journal = scratch.canonical;
        if (!capture.valid || !semantic.valid || !semantic.complete_global_before_proof ||
            (after ? !semantic.all_after : !semantic.all_before) || semantic.inode != reclaim_key_.inode ||
            semantic.old_generation != reclaim_key_.generation || semantic.active_sequence != reclaim_journal_.sequence ||
            semantic.parsed_next_sequence != reclaim_journal_.sequence + 1 ||
            semantic.cleanup_sequence != reclaim_journal_.sequence + 2 ||
            !scratch.reclaim.valid || !same_reclaim(scratch.reclaim.reclaim, homes) ||
            !journal.ready || journal.sequence != reclaim_journal_.sequence || journal.count != reclaim_journal_.count ||
            journal.journal_head != reclaim_journal_.journal_head || journal.descriptor != reclaim_journal_.descriptor ||
            journal.commit != reclaim_journal_.commit || journal.end != reclaim_journal_.end)
            return Status::fault("orphan device reconstruction differs from original publication");
        for (usize index = 0; index < homes.count; ++index)
            if (journal.targets[index] != reclaim_journal_.targets[index] ||
                journal.payloads[index] != reclaim_journal_.payloads[index])
                return Status::fault("orphan actual journal payload differs from original publication");
        if (scratch.superblock != reclaim_storage_->superblock || scratch.descriptors != reclaim_storage_->descriptors ||
            scratch.inode_bitmaps != reclaim_storage_->inode_bitmaps || scratch.block_bitmaps != reclaim_storage_->block_bitmaps)
            return Status::fault("orphan complete original controls differ");
        for (usize group = 0; group < 4; ++group)
            if (reclaim_capture_.initialized_[group] && scratch.tables[group] != reclaim_storage_->inode_tables[group])
                return Status::fault("orphan complete original inode tables differ");
        if (reclaim_disk_capture_.directory_count_ != reclaim_capture_.directory_count_)
            return Status::fault("orphan original directory count differs");
        for (usize index = 0; index < reclaim_capture_.directory_count_; ++index)
        {
            const auto &old = reclaim_storage_->directories[index], &observed = storage.directories[index];
            if (old.inode != observed.inode || old.logical != observed.logical || old.physical != observed.physical || old.bytes != observed.bytes)
                return Status::fault("orphan complete original directory differs");
        }
        reclaim_empty_head_ = reclaim_journal_.journal_head;
        const auto be32 = [&](usize at, u32 value) {
            for (usize byte = 0; byte < 4; ++byte) reclaim_empty_head_[at + byte] = u8(value >> (24 - 8 * byte));
        };
        be32(24, semantic.cleanup_sequence); be32(28, 0); be32(252, 0);
        be32(252, jbd2::detail::crc32c(~u32{0}, reclaim_empty_head_));
        return reclaim_guard();
    }
    [[nodiscard]] const Block *captured_orphan_block(u64 physical) const
    {
        if (!reclaim_storage_) return nullptr;
        const auto &storage = *reclaim_storage_; const auto &binding = reclaim_capture_.binding_;
        if (physical == 1) return &storage.superblock;
        if (physical == 2) return &storage.descriptors;
        if (physical == binding.layout.journal_first_block) return &storage.journal;
        for (usize group = 0; group < 4; ++group)
        {
            const auto *metadata = binding.metadata.data() + group * 4;
            if (physical == metadata[1].first) return &storage.block_bitmaps[group];
            if (physical == metadata[2].first) return &storage.inode_bitmaps[group];
            if (reclaim_capture_.initialized_[group] && physical >= metadata[3].first &&
                physical - metadata[3].first < 512) return &storage.inode_tables[group][physical - metadata[3].first];
        }
        for (usize index = 0; index < reclaim_capture_.directory_count_; ++index)
            if (physical == storage.directories[index].physical) return &storage.directories[index].bytes;
        return nullptr;
    }
    // Cache lock is held; no device I/O occurs. Reconcile every resident
    // participant before pinning can evict a disagreeing canonical version.
    [[nodiscard]] Status reconcile_orphan_cache() const
    {
        if (cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
            cache_.report_.read_pins || cache_.report_.transaction_active || cache_.transaction_)
            return Status::busy("orphan cache has retained work");
        const auto &homes = reclaim_plan_.reclaim;
        for (const auto &entry : cache_.entries_)
        {
            if (entry.report.state != MetadataBufferState::ready) continue;
            const Block *before{};
            if (entry.report.block == reclaim_capture_.binding_.layout.journal_first_block) before = &reclaim_storage_->journal;
            for (usize index = 0; index < homes.count; ++index)
                if (entry.report.block == homes.targets[index]) before = &homes.before[index];
            if (!before) continue;
            if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty || !entry.report.generation)
                return Status::fault("orphan resident participant identity differs");
            for (usize byte = 0; byte < 1024; ++byte)
                if (entry.canonical[byte] != std::byte((*before)[byte]))
                    return Status::fault("orphan resident participant bytes differ");
        }
        return {};
    }
    // Mounted maintenance selects the legacy regular-only admission without a
    // separate cache read that could consume persistent generation capacity.
    template <bool AllowDirectory, bool CheckedOwnership = false, bool CheckedMode = false>
    [[nodiscard]] Status prepare_for(Ext4InodeMetadataUpdate update,
                                      u32 data_block = 0, const Block *data_image = nullptr)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public mutation");
            if (report_.phase != Ext4OwnedPhase::open) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            // Rejected admission cannot change an already sealed transaction.
            if (data_image)
            { has_data_ = true; data_block_ = data_block; data_image_ = *data_image; }
            report_.phase = Ext4OwnedPhase::preparing;
        }
        if (auto status = writer_.template prepare<AllowDirectory, CheckedOwnership, CheckedMode>(cache_.session_, bounds_, update); !status)
            return reject(status);
        blocks_ = {1, writer_.report().target_block, writer_.journal_first_, has_data_ ? data_block_ : 0};
        if (has_data_ && (data_block_ == blocks_[0] || data_block_ == blocks_[1] || data_block_ == blocks_[2]))
            return reject(Status::invalid_argument("ordered-data block aliases metadata"));
        std::array<Block, 3> metadata_images{};
        if (auto status = writer_.logical_images(metadata_images); !status) return fail(status);
        for (usize index = 0; index < metadata_images.size(); ++index) sealed_[index] = metadata_images[index];
        if (has_data_) sealed_[3] = data_image_;
        participant_count_ = has_data_ ? 4 : 3;
        if constexpr (CheckedOwnership || CheckedMode)
        {
            completion_.policy_ = CheckedOwnership ? Ext4ReplayPolicy::checked_ownership : Ext4ReplayPolicy::checked_mode;
            completion_.preview_.policy_ = completion_.policy_;
        }
        completion_.preview_.allow_ordered_data_ = !CheckedOwnership && !CheckedMode && (has_data_ || mounted_dirty_);
        return stage_prepared();
    }
    // Owner preparation excludes public admissions. Check every resident old
    // participant before a later pin can evict a disagreeing cached version.
    [[nodiscard]] Status reconcile_directory_cache() const
    {
        if (cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
            cache_.report_.read_pins || cache_.report_.transaction_active || cache_.transaction_)
            return Status::busy("directory candidate cache has retained work");
        for (const auto &entry : cache_.entries_)
        {
            if (entry.report.state != MetadataBufferState::ready) continue;
            const auto *before = create_writer_.original_preimage(entry.report.block);
            if (!before) continue;
            if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty ||
                !entry.report.generation)
                return Status::fault("directory candidate resident preimage identity differs");
            for (usize byte = 0; byte < before->size(); ++byte)
                if (entry.canonical[byte] != std::byte((*before)[byte]))
                    return Status::fault("directory candidate resident preimage bytes differ");
        }
        return {};
    }
    // Rebase only the successfully initialized unallocated data preimage.
    // Slot identities, pins, transaction serial and the original Session remain.
    [[nodiscard]] Status stage_initialized_directory()
    {
        if (!create_writer_.initialization_complete_ || !create_writer_.initialization_required_)
            return Status::bad_descriptor("directory initialization has no strict final plan");
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            if (auto checked = prepared(true); !checked) return checked;
            usize data = participant_count_;
            for (usize index = 0; index < participant_count_; ++index)
                if (blocks_[index] == create_writer_.candidate_block_) { data = index; break; }
            if (data == participant_count_) return Status::fault("directory candidate retained pin absent");
            auto *entry = cache_.find(pins_[data]);
            if (!entry || entry->report.generation > ~u64{0} - 3)
                return Status::overflow("directory initialized version reservation exhausted");
            for (usize byte = 0; byte < 1024; ++byte)
                if (entry->canonical[byte] != std::byte(create_writer_.old_directory_data_[byte]))
                    return Status::fault("directory initialized old canonical preimage differs");
            for (auto &byte : entry->canonical) byte = std::byte{0};
            pins_[data].generation_ = ++entry->report.generation;
        }
        // The final planner must retain exactly the admitted participant names.
        // Reuse permanent Owner image storage; no large stack image array.
        for (usize index = 0; index < participant_count_; ++index)
        {
            const u64 block = index ? create_writer_.plan_.targets[index - 1] : create_writer_.journal_first_;
            if (blocks_[index] != block) return Status::fault("directory final participant selection differs");
            sealed_[index] = index ? create_writer_.plan_.after[index - 1] : create_writer_.journal_;
            if (status = transaction_.edit(blocks_[index], 0, std::as_bytes(std::span{sealed_[index]})); !status)
                return status;
        }
        { smp::ScopedSpinLock guard(cache_.lock_); status = prepared(true); }
        return status;
    }

    [[nodiscard]] Status stage_prepared(bool cold_namespace = false)
    {
        if (!participant_count_ || participant_count_ > participant_capacity || participant_count_ > BufferCapacity)
            return reject(Status::no_memory("owned ext4 canonical participant capacity"));
        if (create_active_ && create_writer_.directory_create_)
        {
            Status status;
            { smp::ScopedSpinLock guard(cache_.lock_); status = reconcile_directory_cache(); }
            if (!status) return reject(status);
        }
        if (regular_move_active_)
        {
            Status checked;
            { smp::ScopedSpinLock guard(cache_.lock_); checked = reconcile_regular_move_cache(); }
            if (!checked) return reject(checked);
        }
        for (usize index = 0; index < participant_count_; ++index)
        {
            Status pinned;
            if (startup_regular_move_io_.active)
            {
                StartupPinReadGuard guard(*this, blocks_[index], pins_[index]);
                pinned = cache_.pin(blocks_[index], pins_[index], guard);
                if (pinned) pinned = startup_regular_move_adopt_cache_read();
            }
            else pinned = cache_.pin(blocks_[index], pins_[index]);
            if (!pinned) return reject(pinned);
        }
        std::array<Pin *, participant_capacity> pins{};
        for (usize index = 0; index < participant_count_; ++index) pins[index] = &pins_[index];
        if (auto status = cache_.begin(std::span<Pin *const>{pins}.first(participant_count_), transaction_); !status)
            return reject(status);
        if (create_active_ && create_writer_.directory_create_)
        {
            Status checked;
            {
            smp::ScopedSpinLock guard(cache_.lock_);
            for (usize index = 0; index < participant_count_; ++index)
            {
                const auto *entry = cache_.find(pins_[index]);
                const auto *before = create_writer_.original_preimage(blocks_[index]);
                if (!entry || !before)
                { checked = Status::fault("directory staged preimage identity absent"); break; }
                for (usize byte = 0; byte < 1024; ++byte)
                    if (entry->canonical[byte] != std::byte((*before)[byte]))
                    { checked = Status::fault("directory staged preimage bytes differ"); break; }
                if (!checked) break;
            }
            }
            if (!checked) return fail(checked);
        }
        if (regular_move_active_)
        {
            Status checked;
            { smp::ScopedSpinLock guard(cache_.lock_); checked = regular_move_pinned_preimages(); }
            if (!checked) return fail(checked);
        }
        for (usize index = 0; index < participant_count_; ++index)
            if (auto status = transaction_.edit(blocks_[index], 0, std::as_bytes(std::span{sealed_[index]})); !status)
                return fail(status);
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = prepared(false);
        }
        if (!status) return reject(status);
        if (cold_namespace)
        {
            // Phase remains preparing while pins are collected; no public cache
            // reader or commit may enter before the canonical authority seals.
            { smp::ScopedSpinLock guard(cache_.lock_); status = prepared(true); if (status) activate(); }
            if (!status) return fail(status);
            return finish(Ext4OwnedPhase::durable, {});
        }
        if (directory_io_.active)
            if (auto checked = directory_io_endpoint(Ext4DirectoryInitializationBudget::preparation_reads, 0, 0); !checked)
                return fail(checked);
        return finish(Ext4OwnedPhase::prepared, {});
    }

  public:
    [[nodiscard]] Status commit()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("startup or typed child excludes public commit");
            if (report_.phase != Ext4OwnedPhase::prepared) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            smp::ScopedSpinLock cache_guard(cache_.lock_);
            if (cache_.report_.read_pins != participant_count_) return Status::busy("owned ext4 retained canonical readers");
            report_.phase = Ext4OwnedPhase::publishing;
        }
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = prepared(true);
        }
        if (!status) return fail(status);
        if (create_active_ && create_writer_.directory_create_)
        {
            if (status = directory_io_exact(); !status) return fail(status);
            if (status = create_writer_.revalidate_directory(false); !status) return fail(status);
            // The ownership scan itself performs I/O. Freeze its observed
            // namespace/allocation participants again before the first zero write.
            if (status = create_writer_.revalidate_directory(false, false); !status) return fail(status);
            if (create_writer_.initialization_required_)
            {
                if (status = create_writer_.initialize_directory(); !status) return fail(status);
                if (status = stage_initialized_directory(); !status) return fail(status);
            }
            // Deferred proposed replay is read-only but can observe drift. The
            // final physical bookend precedes the first journal FLUSH/WRITE.
            if (status = create_writer_.revalidate_directory(create_writer_.initialization_required_, false); !status)
                return fail(status);
            const u64 initialized = create_writer_.initialization_required_ ? 1 : 0;
            if (status = directory_io_endpoint(Ext4DirectoryInitializationBudget::initialization_reads,
                    initialized, initialized); !status) return fail(status);
        }
        if (has_data_)
        {
            const usize data_participant = (first_data_active_ || next_data_active_) ? 5 : 3;
            // Submit the exact block/image checked against its canonical pin;
            // scratch copies cannot supply different bytes or a different LBA.
            if (status = cache_.session_.write_blocks(blocks_[data_participant] * 2,
                    std::as_bytes(std::span{sealed_[data_participant]})); !status) return fail(status);
            if (status = cache_.session_.flush(); !status) return fail(status);
        }
        // Recheck the sealed participant generations after ordered data I/O.
        { smp::ScopedSpinLock guard(cache_.lock_); status = prepared(true); }
        if (!status) return fail(status);
        if (status = next_data_active_ ? next_data_writer_.publish_prepared() :
                     first_data_active_ ? first_data_writer_.publish_prepared() :
                     create_active_ ? create_writer_.publish_prepared() :
                     regular_move_active_ ? regular_move_writer_.publish_prepared() :
                     namespace_active_ ? namespace_writer_.publish_prepared() :
                     rename_active_ ? rename_writer_.publish_prepared() : writer_.publish_prepared(); !status)
            return fail(status);
        if (directory_io_.active)
            if (status = directory_io_endpoint(0, create_writer_.home_count_ + 4, 4); !status) return fail(status);
        // Only this exact successful invocation can activate private authority.
        if (!active_writer().published()) return fail(Status::fault("owned ext4 publication durability"));
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = prepared(true);
            if (status)
            {
                activate();
            }
        }
        if (!status) return fail(status);
        return finish(Ext4OwnedPhase::durable, {});
    }

    [[nodiscard]] Status checkpoint() { return checkpoint_original(false); }

  private:
    [[nodiscard]] Status checkpoint_original(bool startup_regular_move)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_regular_move)
            {
                if (!startup_active_ || !startup_regular_move_io_.active || !regular_move_recovery_active_ ||
                    typed_native_public_blocked() || startup_report_.initial_family != Ext4StartupFamily::regular_move_active ||
                    startup_report_.phase != Ext4StartupPhase::regular_move_checkpointing)
                    return Status::fault("cold regular move checkpoint lacks private original admission");
            }
            else if (startup_active_ || typed_public_blocked()) return Status::busy("startup or typed child excludes public checkpoint");
            if (report_.phase != Ext4OwnedPhase::durable) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            smp::ScopedSpinLock cache_guard(cache_.lock_);
            if (cache_.report_.read_pins != participant_count_) return Status::busy("owned ext4 retained canonical readers");
            report_.phase = Ext4OwnedPhase::checkpointing;
        }
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = authorized();
        }
        if (!status) return fail(status);
        if (directory_io_.active)
            if (status = directory_io_exact(); !status) return fail(status);
        if (regular_move_active_)
        {
            if (!regular_move_recovery_active_)
            {
                if (status = regular_move_completion_.prepare(cache_.session_, bounds_); !status) return fail(status);
                const auto &observed = regular_move_completion_.binding_;
                if (observed.super_ != regular_move_writer_.super_ || observed.descriptors_ != regular_move_writer_.descriptors_ ||
                    observed.inode_bitmaps_ != regular_move_writer_.inode_bitmaps_ ||
                    observed.block_bitmaps_ != regular_move_writer_.block_bitmaps_ ||
                    observed.journal_table_block_ != regular_move_writer_.journal_table_block_)
                    return fail(Status::fault("regular move completion ancillary history differs from original writer"));
                const Block *journal_table = &regular_move_writer_.journal_table_;
                for (usize index = 0; index < observed.validation_.count; ++index)
                    if (observed.scratch_.canonical.targets[index] == observed.journal_table_block_)
                        journal_table = observed.validation_.home_after[index] ?
                            &regular_move_writer_.scratch_.regular_move_plan.after[index] :
                            &regular_move_writer_.scratch_.regular_move_plan.before[index];
                if (observed.journal_table_ != *journal_table)
                    return fail(Status::fault("regular move completion original journal-inode table differs"));
                if (status = regular_move_completion_.matches_published(regular_move_writer_.journal_plan(),
                        regular_move_writer_.canonical_plan()); !status) return fail(status);
            }
            if (status = regular_move_completion_.matches(blocks_, sealed_, participant_count_, authority_.sequence); !status)
                return fail(status);
            { smp::ScopedSpinLock guard(cache_.lock_); status = authorized(); }
            if (!status) return fail(status);
            if (status = regular_move_completion_.execute(); !status) return fail(status);
            if (!regular_move_completion_.report().complete())
                return fail(Status::fault("owned regular move checkpoint durability"));
        }
        else if (namespace_active_)
        {
            if (!namespace_recovery_active_)
            {
                if (status = namespace_completion_.prepare(cache_.session_, bounds_); !status) return fail(status);
                if (status = namespace_completion_.matches_published(namespace_writer_.journal_,
                        namespace_writer_.scratch_.namespace_plan); !status) return fail(status);
            }
            if (status = namespace_completion_.matches(blocks_, sealed_, participant_count_, authority_.sequence); !status)
                return fail(status);
            // Revalidate the original publication seal after proof reads. No
            // caller-built result may substitute for its cache/session epoch.
            { smp::ScopedSpinLock guard(cache_.lock_); status = authorized(); }
            if (!status) return fail(status);
            if (status = namespace_completion_.execute(); !status) return fail(status);
            if (!namespace_completion_.report().complete())
                return fail(Status::fault("owned namespace checkpoint durability"));
        }
        else
        {
            if (create_active_)
                if (status = create_writer_.matches_published(); !status) return fail(status);
            if (status = completion_.run(cache_.session_, bounds_); !status) return fail(status);
            if (!completion_.report().complete()) return fail(Status::fault("owned ext4 checkpoint durability"));
        }
        if (directory_io_.active)
            if (status = directory_io_endpoint(Ext4DirectoryInitializationBudget::checkpoint_reads,
                    create_writer_.home_count_ + 1, 5); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = authorized();
            if (status)
            {
                // Exact clean controls come from this completed same-session run.
                if (regular_move_active_) sealed_[0] = regular_move_completion_.binding_.empty_head_;
                else if (namespace_active_) sealed_[0] = namespace_completion_.journal_;
                else if (rename_active_ || create_active_ || first_data_active_ || next_data_active_)
                    sealed_[0] = completion_.journal_;
                else { sealed_[0] = completion_.filesystem_; sealed_[2] = completion_.journal_; }
                publish(sealed_);
                authority_.active = false;
            }
        }
        if (!status) return fail(status);
        if (status = retire(); !status) return fail(status);
        return finish(namespace_active_ && namespace_completion_.orphan_cleanup_pending() ?
            Ext4OwnedPhase::orphan_cleanup_pending : Ext4OwnedPhase::checkpointed, {});
    }

  public:
    [[nodiscard]] Status discard()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("startup domain excludes public discard");
            if (report_.phase != Ext4OwnedPhase::prepared) return phase_error();
            if (admissions_) return Status::busy("owned ext4 reader admission in progress");
            report_.phase = Ext4OwnedPhase::discarding;
        }
        if (directory_io_.active)
            if (auto status = directory_io_exact(); !status) return fail(status);
        if (auto status = retire(); !status) return fail(status);
        return finish(Ext4OwnedPhase::discarded, {});
    }
    [[nodiscard]] Status close()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (startup_active_ || typed_public_blocked()) return Status::busy("startup domain excludes public close");
            if (report_.phase != Ext4OwnedPhase::open && report_.phase != Ext4OwnedPhase::checkpointed &&
                report_.phase != Ext4OwnedPhase::discarded && report_.phase != Ext4OwnedPhase::rejected)
                return phase_error();
            if (admissions_ || cache_.report().read_pins) return Status::busy("owned ext4 retained readers");
            report_.phase = Ext4OwnedPhase::closing;
        }
        if (auto status = cache_.close(); !status) return fail(status);
        return finish(Ext4OwnedPhase::closed, {});
    }
    [[nodiscard]] Ext4OwnedReport report() const
    { smp::ScopedSpinLock guard(lock_); return report_; }
    [[nodiscard]] MetadataOwnerReport buffers() const { return cache_.report(); }

  private:
    // Mounted admission uses actual private records before close/seal I/O.
    // Owner then Cache lock order matches rearm; no callback or transport runs.
    [[nodiscard]] Status mounted_teardown_preflight() const
    {
        smp::ScopedSpinLock guard(lock_);
        if (typed_public_blocked() || startup_active_ || startup_route_opening_ || startup_seal_.active || startup_namespace_seal_.active ||
            (startup_workspace_ && startup_report_.phase != Ext4StartupPhase::complete) || admissions_ || authority_.active)
            return Status::busy("owned ext4 has active admission or startup work");
        if (report_.phase == Ext4OwnedPhase::failed)
            return report_.status ? Status::fault("owned ext4 failed without an error") : report_.status;
        if (report_.phase != Ext4OwnedPhase::open) return phase_error();
        const auto *namespace_result = namespace_completion_.admission_.result();
        if (report_.orphan_cleanup_pending ||
            (!reclaim_completed_ && (namespace_completion_.orphan_cleanup_pending() ||
             (namespace_result && namespace_result->homes.orphan_published))) ||
            rename_active_ || create_active_ || first_data_active_ || next_data_active_ ||
            namespace_active_ || namespace_recovery_active_ || regular_move_active_ || regular_move_recovery_active_ ||
            startup_regular_move_io_.active || participant_count_ || has_data_ ||
            reclaim_prepared_ || reclaim_active_ || reclaim_published_ || reclaim_completed_ ||
            reclaim_storage_ || reclaim_scratch_ || reclaim_lifetime_ || reclaim_ticket_.valid())
            return Status::busy("owned ext4 retains an operation or orphan obligation");
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        if (cache_.report_.state != MetadataOwnerState::ready)
            return {cache_.report_.first_error == StatusCode::ok ? StatusCode::fault : cache_.report_.first_error,
                    "owned ext4 canonical owner is not ready"};
        if (cache_.report_.in_flight || cache_.report_.read_pins || cache_.report_.transaction_active ||
            cache_.transaction_ || transaction_.active_)
            return Status::busy("owned ext4 canonical operation remains active");
        for (const auto &pin : pins_)
            if (pin.active_) return Status::busy("owned ext4 retains a participant pin");
        for (const auto &entry : cache_.entries_)
        {
            if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty ||
                entry.report.state == MetadataBufferState::loading)
                return Status::busy("owned ext4 canonical slot retains work");
            if (entry.report.state != MetadataBufferState::unused && entry.report.state != MetadataBufferState::ready &&
                entry.report.state != MetadataBufferState::exhausted)
                return Status::fault("owned ext4 canonical slot is not drained");
        }
        const auto session = cache_.session_.report();
        if (!session) return session.status();
        if (session.value().state != block::DeviceState::owned ||
            session.value().identity != cache_.report_.device_identity ||
            session.value().registration_generation != cache_.report_.registration_generation ||
            session.value().session_serial != cache_.report_.session_serial)
            return Status::fault("owned ext4 original Session differs before teardown");
        if (session.value().in_flight || session.value().pending_writes)
            return Status::busy("owned ext4 original Session retains I/O");
        return {};
    }
    // Only the mounted owner may recycle drained private operation objects.
    // Session, cache generations and transaction serial never restart.
    [[nodiscard]] Status rearm()
    {
        if (common_opening_) return Status::busy("common opening excludes unrelated rearm");
        return rearm_original();
    }
    [[nodiscard]] Status startup_rearm()
    {
        if (common_opening_)
        {
            if (!startup_active_ || common_typed_selected() || common_opening_->phase_ != CommonOpeningPhase::selected)
                return Status::busy("common startup rearm lacks its selected original continuation");
            if (auto status = common_retained_exact(); !status) return status;
        }
        return rearm_original();
    }
    [[nodiscard]] Status rearm_original()
    {
        smp::ScopedSpinLock guard(lock_);
        if (typed_renewal_quarantined_) return Status::fault("typed owner renewal is permanently quarantined");
        if (!typed_workspace_anchors_exact_locked())
            return quarantine_typed_renewal_locked(Status::fault("typed owner permanent workspace anchor changed before rearm"));
        if (typed_native_public_blocked()) return Status::busy("typed child excludes rearm");
        if (report_.phase != Ext4OwnedPhase::checkpointed && report_.phase != Ext4OwnedPhase::discarded &&
            report_.phase != Ext4OwnedPhase::rejected) return phase_error();
        const auto cache = cache_.report();
        if (directory_io_.active)
            if (auto status = directory_io_exact(); !status)
                return quarantine_typed_renewal_locked(status);
        if (startup_regular_move_io_.active || admissions_ || authority_.active || cache.state != MetadataOwnerState::ready ||
            cache.in_flight || cache.read_pins || cache.transaction_active)
            return Status::busy("owned ext4 operation has retained work");
        if (typed_attempted_)
        {
            auto &workspace = typed_attachment_->workspace_;
            if (!workspace.zero_.owner_retirement_idle() || !workspace.allocated_.owner_retirement_idle())
                return quarantine_typed_renewal_locked(Status::fault("typed child rearm lacks retired callbacks"));
            auto status = workspace.allocated_route_ ? workspace.allocated_.idle() : workspace.zero_.idle();
            if (!status) return quarantine_typed_renewal_locked(status);
            smp::ScopedSpinLock cache_guard(cache_.lock_);
            if (auto exact = typed_published_cache_exact_locked(workspace); !exact) return exact;
        }
        return typed_rearm_drained_locked();
    }
    // Caller holds Owner lock and has proved actual terminal retirement.
    [[nodiscard]] Status typed_rearm_drained_locked()
    {
        auto reset = []<typename T>(T &object) { object.~T(); ::new (static_cast<void *>(&object)) T; };
        writer_.~Ext4MetadataTransaction();
        ::new (static_cast<void *>(&writer_)) Ext4MetadataTransaction(mounted_dirty_);
        rename_writer_.~Ext4RenameTransaction();
        ::new (static_cast<void *>(&rename_writer_)) Ext4RenameTransaction();
        create_writer_.~Ext4CreateTransaction();
        ::new (static_cast<void *>(&create_writer_)) Ext4CreateTransaction();
        first_data_writer_.~Ext4FirstDataTransaction();
        ::new (static_cast<void *>(&first_data_writer_)) Ext4FirstDataTransaction();
        next_data_writer_.~Ext4NextDataTransaction();
        ::new (static_cast<void *>(&next_data_writer_)) Ext4NextDataTransaction();
        namespace_writer_.~Ext4NamespaceTransaction();
        ::new (static_cast<void *>(&namespace_writer_)) Ext4NamespaceTransaction();
        regular_move_writer_.~Ext4RegularMoveTransaction();
        ::new (static_cast<void *>(&regular_move_writer_)) Ext4RegularMoveTransaction();
        regular_move_completion_.~Ext4RegularMoveCompletion();
        ::new (static_cast<void *>(&regular_move_completion_)) Ext4RegularMoveCompletion();
        namespace_completion_.~Ext4NamespaceCompletion();
        ::new (static_cast<void *>(&namespace_completion_)) Ext4NamespaceCompletion();
        completion_.~Completion();
        ::new (static_cast<void *>(&completion_)) Completion(mounted_dirty_, mounted_dirty_);
        reset(transaction_);
        for (auto &pin : pins_) reset(pin);
        reset(authority_);
        directory_io_ = {};
        blocks_ = {}; sealed_ = {}; data_image_ = {}; data_block_ = 0; participant_count_ = 0;
        has_data_ = rename_active_ = create_active_ = first_data_active_ = next_data_active_ = namespace_active_ = false;
        namespace_recovery_active_ = regular_move_active_ = regular_move_recovery_active_ = false;
        reset(startup_regular_move_io_);
        reset(reclaim_capture_); reset(reclaim_disk_capture_);
        reclaim_plan_ = {}; reclaim_check_ = {}; reclaim_format_ = {}; reclaim_journal_ = {};
        reclaim_storage_ = nullptr; reclaim_scratch_ = nullptr; reclaim_lifetime_ = nullptr;
        reclaim_ticket_ = {}; reclaim_key_ = {};
        if (!startup_active_ || !startup_reclaim_seal_.minted) reclaim_io_ = {};
        reclaim_empty_head_ = {};
        reclaim_prepared_ = reclaim_active_ = reclaim_published_ = reclaim_completed_ = false;
        typed_attempted_ = typed_completed_ = false; typed_context_ = nullptr;
        report_ = {}; report_.phase = Ext4OwnedPhase::open;
        return {};
    }
    [[nodiscard]] Result<Ext4RecoveryInfo> retained_orphan_binding(Ext4InodeLifetimeKey key) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4OwnedPhase::orphan_cleanup_pending || !namespace_active_ ||
            !namespace_completion_.report().complete() || !namespace_completion_.orphan_cleanup_pending() ||
            key.session != cache_.report().session_serial ||
            !namespace_completion_.admission_.result() ||
            namespace_completion_.admission_.result()->removed_inode != key.inode)
            return Status::bad_descriptor("retained orphan binding lacks original namespace completion");
        const auto *original = namespace_writer_.binding_.info();
        if (!original) return Status::fault("retained orphan original binding absent");
        const usize offset = ((key.inode - 1) % 4) * 256;
        if (ext4_checked::le32(namespace_writer_.destination_, offset + 100) != key.generation)
            return Status::bad_descriptor("retained orphan generation differs from original victim");
        auto value = *original;
        value.empty_log = true; value.journal_sequence = namespace_completion_.report().journal_sequence;
        return value;
    }
    [[nodiscard]] driver::BlockDevice &bound_device() { return cache_.session_; }
    [[nodiscard]] Result<block::SessionReport> session_report() const { return cache_.session_.report(); }
    struct DirectoryIo final
    {
        const Ext4OwnedTransaction *owner{};
        const typename Registry::Session *session{};
        block::SessionBindingObservation binding{}, binding_seal{};
        block::SessionReport baseline{}, baseline_seal{}, progress{}, progress_seal{};
        bool active{};
    };
    [[nodiscard]] static bool directory_io_healthy(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    [[nodiscard]] Status reserve_directory_io()
    {
        if (directory_io_.active) return Status::already_exists("directory I/O reservation is one-shot");
        const auto binding = cache_.session_.observe_binding();
        const auto actual = cache_.session_.report();
        if (!binding || !actual) return binding ? actual.status() : binding.status();
        if (!directory_io_healthy(actual.value()))
            return Status::fault("directory I/O reservation requires completed original Session parity");
        const auto &b = binding.value();
        if (b.session != &cache_.session_ || b.sector_bytes != 512 || !b.writable || b.maximum_transfer < 1024)
            return Status::unsupported("directory I/O reservation requires writable bounded original Session");
        if (auto status = Ext4DirectoryInitializationBudget::reserve(actual.value()); !status) return status;
        directory_io_.owner = this; directory_io_.session = &cache_.session_;
        directory_io_.binding = directory_io_.binding_seal = b;
        directory_io_.baseline = directory_io_.baseline_seal = actual.value();
        directory_io_.progress = directory_io_.progress_seal = actual.value();
        directory_io_.active = true;
        return {};
    }
    [[nodiscard]] Status directory_io_retained() const
    {
        if (!directory_io_.active || directory_io_.owner != this || directory_io_.session != &cache_.session_ ||
            directory_io_.binding != directory_io_.binding_seal ||
            !exact_session(directory_io_.baseline, directory_io_.baseline_seal) ||
            !exact_session(directory_io_.progress, directory_io_.progress_seal))
            return Status::fault("directory I/O retained Owner or Session identity differs");
        const auto binding = cache_.session_.observe_binding();
        if (!binding) return binding.status();
        if (binding.value() != directory_io_.binding)
            return Status::fault("directory I/O original Session volume binding differs");
        return {};
    }
    [[nodiscard]] Status directory_io_exact() const
    {
        if (auto status = directory_io_retained(); !status) return status;
        const auto actual = cache_.session_.report();
        if (!actual) return actual.status();
        return directory_io_healthy(actual.value()) && exact_session(actual.value(), directory_io_.progress) ?
            Status{} : Status::fault("directory I/O exact admitted endpoint differs");
    }
    [[nodiscard]] Status directory_io_endpoint(u64 reads, u64 writes, u64 flushes)
    {
        if (auto status = directory_io_retained(); !status) return status;
        const auto actual = cache_.session_.report();
        if (!actual) return actual.status();
        const auto &now = actual.value(); const auto &old = directory_io_.progress;
        const auto &base = directory_io_.baseline;
        if (!directory_io_healthy(now) || now.identity != old.identity ||
            now.registration_generation != old.registration_generation || now.session_serial != old.session_serial ||
            now.reads < old.reads || now.reads - old.reads > reads ||
            now.writes < old.writes || now.writes - old.writes != writes ||
            now.flushes < old.flushes || now.flushes - old.flushes != flushes ||
            now.reads < base.reads || now.reads - base.reads > Ext4DirectoryInitializationBudget::whole_reads ||
            now.writes < base.writes || now.writes - base.writes > Ext4DirectoryInitializationBudget::whole_writes ||
            now.flushes < base.flushes || now.flushes - base.flushes > Ext4DirectoryInitializationBudget::whole_flushes)
            return Status::fault("directory I/O bounded physical endpoint differs");
        directory_io_.progress = directory_io_.progress_seal = now;
        return {};
    }
    // Mounted post-checkpoint verification is read-only and retains this Owner
    // and Session. It must be accounted before drained rearm resets the guard.
    [[nodiscard]] Status directory_continuation_end()
    {
        return directory_io_.active ? directory_io_endpoint(
            Ext4DirectoryInitializationBudget::continuation_reads, 0, 0) : Status{};
    }
    [[nodiscard]] Status phase_error() const
    {
        if (report_.phase == Ext4OwnedPhase::failed) return Status::denied("owned ext4 is quarantined");
        if (report_.phase == Ext4OwnedPhase::unused || report_.phase == Ext4OwnedPhase::closed)
            return Status::not_initialized("owned ext4 is not open");
        return Status::busy("owned ext4 operation is not admitted in this phase");
    }
    [[nodiscard]] Status stable() const
    {
        if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public pins");
        switch (report_.phase)
        {
            case Ext4OwnedPhase::open: case Ext4OwnedPhase::prepared: case Ext4OwnedPhase::durable:
            case Ext4OwnedPhase::checkpointed: case Ext4OwnedPhase::discarded: case Ext4OwnedPhase::rejected:
            case Ext4OwnedPhase::orphan_cleanup_pending: return {};
            default: return phase_error();
        }
    }
    [[nodiscard]] Status finish(Ext4OwnedPhase phase, Status status)
    {
        smp::ScopedSpinLock guard(lock_);
        report_.phase = phase; report_.status = status;
        report_.writer = active_writer();
        report_.completion = regular_move_active_ ? Ext4CompletionReport{} :
            namespace_active_ ? namespace_completion_.report() : completion_.report();
        report_.regular_move = regular_move_active_; report_.cold_regular_move = regular_move_recovery_active_;
        report_.regular_move_completion = regular_move_completion_.report();
        if (regular_move_completion_.report().complete())
            report_.completion.journal_sequence = regular_move_completion_.report().empty_sequence;
        if (reclaim_completed_)
            report_.completion.journal_sequence = jbd2::detail::be32(reclaim_empty_head_, 24);
        report_.cold_namespace = namespace_recovery_active_;
        report_.orphan_reclaim_prepared = reclaim_prepared_;
        report_.orphan_reclaimed = reclaim_completed_;
        report_.orphan_cleanup_pending = !reclaim_completed_ && namespace_active_ && namespace_completion_.report().complete() &&
            namespace_completion_.orphan_cleanup_pending();
        return status;
    }
    [[nodiscard]] const Ext4TransactionReport &active_writer() const
    { return next_data_active_ ? next_data_writer_.report() : first_data_active_ ? first_data_writer_.report() :
             create_active_ ? create_writer_.report() : regular_move_active_ ? regular_move_writer_.report() :
             namespace_active_ ? namespace_writer_.report() :
             rename_active_ ? rename_writer_.report() : writer_.report(); }
    [[nodiscard]] Status fail(Status status)
    {
        static_cast<void>(cache_.abort(status));
        return finish(Ext4OwnedPhase::failed, status);
    }
    [[nodiscard]] Status reject(Status status)
    {
        const auto code = status.code();
        if (directory_io_.active && report_.phase == Ext4OwnedPhase::preparing)
            if (auto checked = directory_io_endpoint(Ext4DirectoryInitializationBudget::preparation_reads, 0, 0); !checked)
                return fail(checked);
        const auto session = cache_.session_.report();
        if (!session || session.value().state == block::DeviceState::quarantined ||
            cache_.report().state == MetadataOwnerState::failed ||
            (code != StatusCode::unsupported && code != StatusCode::invalid_argument &&
             code != StatusCode::no_memory && code != StatusCode::no_space && code != StatusCode::not_found &&
             code != StatusCode::already_exists && code != StatusCode::overflow))
            return fail(status);
        if (auto retired = retire(); !retired) return fail(retired);
        return finish(Ext4OwnedPhase::rejected, status);
    }
    [[nodiscard]] Status retire()
    {
        if (transaction_.active_)
            if (auto status = transaction_.discard(); !status) return status;
        for (auto &pin : pins_)
            if (pin.active_)
                if (auto status = pin.release(); !status) return status;
        return {};
    }
    // The cache lock is held; these checks never invoke a transport operation.
    [[nodiscard]] Status prepared(bool quiescent)
    {
        if (cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
            cache_.transaction_ != &transaction_ || transaction_.owner_ != &cache_ || !transaction_.active_ ||
            transaction_.serial_ != cache_.transaction_serial_)
            return Status::fault("owned ext4 private transaction identity");
        if (quiescent && cache_.report_.read_pins != participant_count_)
            return Status::fault("owned ext4 reader quiescence lost");
        for (usize index = 0; index < participant_count_; ++index)
        {
            const auto *entry = cache_.find(pins_[index]);
            if (!entry || entry->report.state != MetadataBufferState::ready || entry->report.block != blocks_[index] ||
                entry->report.transaction_serial != transaction_.serial_ || !entry->report.staged_dirty)
                return Status::fault("owned ext4 canonical participant identity");
            const u64 reserve = create_active_ && create_writer_.initialization_required_ &&
                !create_writer_.initialization_complete_ ? 3 : 2;
            if (entry->report.generation > ~u64{0} - reserve)
                return Status::overflow("owned ext4 version reservation exhausted");
            for (usize byte = 0; byte < 1024; ++byte)
                if (entry->staged[byte] != std::byte(sealed_[index][byte]))
                    return Status::fault("owned ext4 private bytes differ from validated image");
        }
        return {};
    }
    [[nodiscard]] Status authorized()
    {
        if (!authority_.active || authority_.owner != this || authority_.cache != &cache_ ||
            authority_.session != &cache_.session_ || authority_.images != &sealed_ ||
            authority_.identity != cache_.report_.device_identity || authority_.registration != cache_.report_.registration_generation ||
            authority_.session_serial != cache_.report_.session_serial || authority_.transaction != transaction_.serial_ ||
            authority_.sequence != publication_sequence() || !publication_ready() ||
            authority_.domain != publication_domain_ || authority_.first_sector != cache_.report_.first_sector ||
            authority_.sectors != cache_.report_.sectors ||
            cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
            cache_.report_.read_pins != participant_count_ || cache_.transaction_ != &transaction_ ||
            transaction_.owner_ != &cache_ || !transaction_.active_ || transaction_.serial_ != cache_.transaction_serial_)
            return Status::fault("owned ext4 checkpoint authority");
        for (usize index = 0; index < participant_count_; ++index)
        {
            const auto *entry = cache_.find(pins_[index]);
            if (!entry || authority_.slots[index] != pins_[index].slot_ ||
                authority_.generations[index] != pins_[index].generation_ || entry->report.generation == ~u64{0} ||
                entry->report.block != blocks_[index] || entry->report.state != MetadataBufferState::ready ||
                entry->report.transaction_serial != transaction_.serial_)
                return Status::fault("owned ext4 checkpoint version identity");
            for (usize byte = 0; byte < 1024; ++byte)
                if (entry->staged[byte] != std::byte(sealed_[index][byte]) || entry->canonical[byte] != entry->staged[byte])
                    return Status::fault("owned ext4 checkpoint image identity");
        }
        return {};
    }
    [[nodiscard]] u32 publication_sequence() const
    { return reclaim_active_ ? reclaim_journal_.sequence :
        regular_move_recovery_active_ ? regular_move_completion_.report().active_sequence :
        namespace_recovery_active_ ? namespace_completion_.sequence_ : active_writer().sequence; }
    [[nodiscard]] bool publication_ready() const
    {
        if (reclaim_active_) return reclaim_published_;
        if (regular_move_recovery_active_)
        {
            const auto phase = regular_move_completion_.report().phase;
            return phase >= Ext4RegularMoveCompletionPhase::prepared &&
                phase <= Ext4RegularMoveCompletionPhase::complete;
        }
        if (!namespace_recovery_active_) return active_writer().published();
        const auto phase = namespace_completion_.report().phase;
        return phase == Ext4CompletionPhase::preflight || phase == Ext4CompletionPhase::complete;
    }
    // The cache lock is held; activation has no device I/O.
    void activate()
    {
        authority_.owner = this; authority_.cache = &cache_; authority_.session = &cache_.session_;
        authority_.images = &sealed_; authority_.identity = cache_.report_.device_identity;
        authority_.registration = cache_.report_.registration_generation;
        authority_.session_serial = cache_.report_.session_serial;
        authority_.transaction = transaction_.serial_; authority_.domain = publication_domain_;
        authority_.first_sector = cache_.report_.first_sector; authority_.sectors = cache_.report_.sectors;
        authority_.sequence = publication_sequence(); authority_.active = true;
        publish(sealed_);
        for (usize index = 0; index < participant_count_; ++index)
        { authority_.slots[index] = pins_[index].slot_; authority_.generations[index] = pins_[index].generation_; }
    }
    void publish(const std::array<Block, participant_capacity> &images)
    {
        for (usize index = 0; index < participant_count_; ++index)
        {
            auto &entry = cache_.entries_[pins_[index].slot_];
            for (usize byte = 0; byte < 1024; ++byte)
                entry.canonical[byte] = entry.staged[byte] = std::byte(images[index][byte]);
            pins_[index].generation_ = ++entry.report.generation;
        }
    }
    // Embedded guards and handles are destroyed before their original Session.
    #include "ok/fs/ext4_owned_typed_port.inc"
    #include "ok/fs/ext4_owned_mounted_cursor.inc"
    #include "ok/fs/ext4_owned_common_opening.inc"
    Cache cache_{};
    DirectoryIo directory_io_{};
    using Lifetime = Ext4InodeLifetimeTable<128, 512>;
    Ext4OrphanChainCapture<DeviceCapacity> reclaim_capture_{};
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim_plan_{};
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim_check_{};
    Ext4OrphanReclaimJournalScratch reclaim_format_{};
    Ext4OrphanReclaimJournalPlan reclaim_journal_{};
    Ext4OrphanChainCaptureStorage *reclaim_storage_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch *reclaim_scratch_{};
    Lifetime *reclaim_lifetime_{};
    typename Lifetime::Ticket reclaim_ticket_{};
    Ext4InodeLifetimeKey reclaim_key_{};
    bool reclaim_prepared_{}, reclaim_active_{}, reclaim_published_{}, reclaim_completed_{};
    Ext4OrphanReclaimCapture<DeviceCapacity> reclaim_disk_capture_{};
    block::SessionReport reclaim_io_{};
    Ext4ReclaimRouteBudget<typename Registry::Session> startup_route_budget_{};
    bool startup_route_opening_{};
    Block reclaim_empty_head_{}, reclaim_readback_{};
    #include "ok/fs/ext4_mounted_startup_port.inc"

    Ext4MetadataTransaction writer_;
    Ext4RenameTransaction rename_writer_{};
    Ext4CreateTransaction create_writer_{};
    Ext4FirstDataTransaction first_data_writer_{};
    Ext4NextDataTransaction next_data_writer_{};
    Ext4RegularMoveTransaction regular_move_writer_{};
    Ext4RegularMoveCompletion regular_move_completion_{};
    Ext4NamespaceTransaction namespace_writer_{};
    Ext4NamespaceCompletion namespace_completion_{};
    Completion completion_;
    std::array<Pin, participant_capacity> pins_{};
    typename Cache::Transaction transaction_{};
    Ext4RecoveryBounds bounds_{};
    std::array<u64, participant_capacity> blocks_{};
    std::array<Block, participant_capacity> sealed_{};
    Block data_image_{};
    u32 data_block_{};
    usize participant_count_{};
    bool has_data_{};
    bool rename_active_{};
    bool create_active_{};
    bool first_data_active_{};
    bool next_data_active_{};
    bool namespace_active_{};
    bool namespace_recovery_active_{};
    bool regular_move_active_{}, regular_move_recovery_active_{};
    Authority authority_{};
    Ext4OwnedReport report_{};
    usize admissions_{};
    u64 publication_domain_{};
    mutable smp::SpinLock lock_{};
    bool mounted_dirty_{};
};
} // namespace ok::fs
