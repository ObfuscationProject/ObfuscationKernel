#pragma once

#include "ok/block/guarded_reads.hpp"
#include "ok/fs/ext4_reclaim_history.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
