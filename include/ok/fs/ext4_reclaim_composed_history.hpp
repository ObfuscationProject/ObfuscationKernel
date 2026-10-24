#pragma once

#include "ok/fs/ext4_orphan_chain_capture.hpp"
#include "ok/fs/ext4_completion.hpp"
#include "ok/fs/ext4_reclaim_composed_budget.hpp"
#include <new>

namespace ok::fs
{
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
            report.journal_sequence != storage_->rows_[row_count_ - 1].cleanup_sequence || report.checked_inodes > 128)
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
} // namespace ok::fs
