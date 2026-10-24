#pragma once

#include "ok/fs/ext4_orphan_reclaim_capture.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
