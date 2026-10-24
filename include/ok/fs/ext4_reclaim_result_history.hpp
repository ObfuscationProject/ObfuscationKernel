#pragma once

#include "ok/fs/ext4_reclaim_recovery_binding.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
