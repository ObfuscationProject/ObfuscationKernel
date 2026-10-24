#pragma once

#include "ok/block/sessions.hpp"
#include "ok/fs/ext4_orphan_reclaim_cold.hpp"

namespace ok::fs
{
template <typename> class Ext4ZeroTruncateTransaction;
template <typename> class Ext4AllocatedEmptyDataTransaction;
}

namespace ok::fs::reclaim_history
{
// Independent borrow anchors are never copied from a public report or a result seal.
// One drifting pointer cannot redirect rejection to an unrelated live Session.
template <typename Session> class SessionAnchor final
{
    template <typename> friend class ::ok::fs::Ext4ZeroTruncateTransaction;
    template <typename> friend class ::ok::fs::Ext4AllocatedEmptyDataTransaction;
  public:
    SessionAnchor() = default;
    SessionAnchor(const SessionAnchor &) = delete;
    SessionAnchor &operator=(const SessionAnchor &) = delete;
    SessionAnchor(SessionAnchor &&) = delete;
    SessionAnchor &operator=(SessionAnchor &&) = delete;
    [[nodiscard]] Status bind(Session &session)
    {
        if (retired_ || first_ || second_ || third_) return Status::already_exists("original Session anchor is one-shot");
        first_ = second_ = third_ = &session; return {};
    }
    [[nodiscard]] bool exact(const Session *session) const
    { return !retired_ && session && first_ == session && second_ == session && third_ == session; }
    [[nodiscard]] Result<block::SessionBindingObservation> observe_binding() const
        requires requires(Session &session) { session.observe_binding(); }
    {
        Session *original = agreed();
        return original ? original->observe_binding() :
            Result<block::SessionBindingObservation>{Status::fault("original Session anchors have no agreement")};
    }
    // Inert post-quarantine observation uses the independently agreed actual
    // Session, never a drifting transaction pointer or an observation as a lease.
    [[nodiscard]] Result<block::SessionBindingObservation> observe_terminal_binding() const
        requires requires(Session &session) { session.observe_terminal_binding(); }
    {
        Session *original = agreed();
        return original ? original->observe_terminal_binding() :
            Result<block::SessionBindingObservation>{Status::fault("original Session anchors have no agreement")};
    }
    [[nodiscard]] Status abort(Status status) const
    {
        Session *original = agreed();
        return original ? original->abort(status) : Status::fault("original Session anchors have no agreement");
    }
    // Rejection diagnostics use the same independently agreed original for
    // both quarantine/drain and final readback. Minority pointer drift never
    // authorizes further dispatch or exposes an alternate route to the caller.
    [[nodiscard]] Result<block::SessionReport> abort_observe(Status status) const
    {
        Session *original = agreed();
        if (!original) return Status::fault("original Session anchors have no agreement");
        static_cast<void>(original->abort(status));
        return original->report();
    }
  private:
    // Only the concrete failed typed child retires a proven drained original.
    // This removes destruction-time borrows without resetting failed history
    // or making the anchor available for a second Session.
    [[nodiscard]] Status retire_aborted_original(Session &session)
        requires requires(Session &value) { value.observe_terminal_binding(); }
    {
        if (!exact(&session)) return Status::bad_descriptor("terminal retirement lacks its original Session");
        const auto actual = session.report();
        if (!actual) return actual.status();
        const auto &report = actual.value();
        if (report.state != block::DeviceState::quarantined || report.first_error == StatusCode::ok || report.in_flight)
            return Status::busy("terminal retirement requires a quarantined drained Session");
        const auto binding = session.observe_terminal_binding();
        if (!binding) return binding.status();
        if (binding.value().session != &session || binding.value().identity != report.identity ||
            binding.value().registration_generation != report.registration_generation ||
            binding.value().session_serial != report.session_serial)
            return Status::fault("terminal retirement original native binding differs");
        first_ = second_ = third_ = nullptr;
        retired_ = true;
        return {};
    }
    [[nodiscard]] Session *agreed() const
    { return retired_ ? nullptr : first_ == second_ || first_ == third_ ? first_ : second_ == third_ ? second_ : nullptr; }
    Session *first_{}, *second_{}, *third_{};
    bool retired_{};
};

// Compare declared fields, including unused retained entries; never padding.
inline bool same_status(const Status &a, const Status &b)
{
    return a.code() == b.code() && a.message().data() == b.message().data() &&
        a.message().size() == b.message().size();
}
inline bool same_session(const block::SessionReport &a, const block::SessionReport &b)
{
    return a.state == b.state && a.first_error == b.first_error && a.identity == b.identity &&
        a.registration_generation == b.registration_generation && a.session_serial == b.session_serial &&
        a.reads == b.reads && a.writes == b.writes && a.flushes == b.flushes &&
        a.completed_reads == b.completed_reads && a.completed_writes == b.completed_writes &&
        a.completed_flushes == b.completed_flushes && a.in_flight == b.in_flight &&
        a.pending_writes == b.pending_writes;
}
inline bool same_info(const Ext4RecoveryInfo &a, const Ext4RecoveryInfo &b)
{
    if (a.metadata_count > a.metadata.size() || b.metadata_count > b.metadata.size() ||
        a.layout.first_sector != b.layout.first_sector || a.layout.filesystem_blocks != b.layout.filesystem_blocks ||
        a.layout.journal_first_block != b.layout.journal_first_block || a.layout.journal_blocks != b.layout.journal_blocks ||
        a.layout.block_size != b.layout.block_size || a.uuid != b.uuid || a.metadata_count != b.metadata_count ||
        a.journal_inode != b.journal_inode || a.journal_sequence != b.journal_sequence ||
        a.needs_recovery != b.needs_recovery || a.empty_log != b.empty_log) return false;
    for (usize index = 0; index < a.metadata.size(); ++index)
        if (a.metadata[index].first != b.metadata[index].first || a.metadata[index].count != b.metadata[index].count)
            return false;
    return true;
}
inline bool same_result(const Ext4OrphanReclaimColdResult &a, const Ext4OrphanReclaimColdResult &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.targets == b.targets &&
        a.home_after == b.home_after && a.inode == b.inode && a.old_generation == b.old_generation &&
        a.retained_generation == b.retained_generation && a.next_head == b.next_head &&
        a.active_sequence == b.active_sequence && a.parsed_next_sequence == b.parsed_next_sequence &&
        a.cleanup_sequence == b.cleanup_sequence && a.count == b.count && a.old_homes == b.old_homes &&
        a.new_homes == b.new_homes && a.witness_runs == b.witness_runs && a.witness_bytes == b.witness_bytes &&
        a.valid == b.valid && a.all_before == b.all_before && a.all_after == b.all_after &&
        a.complete_global_before_proof == b.complete_global_before_proof;
}
inline bool same_plan(const Ext4OrphanReclaimPlan &a, const Ext4OrphanReclaimPlan &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.inode == b.inode &&
        a.old_generation == b.old_generation && a.retained_generation == b.retained_generation &&
        a.next_head == b.next_head && a.released_blocks == b.released_blocks &&
        a.released_mapping == b.released_mapping && a.targets == b.targets &&
        a.before == b.before && a.after == b.after && a.count == b.count;
}
inline bool same_journal(const Ext4OrphanReclaimJournalPlan &a, const Ext4OrphanReclaimJournalPlan &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.journal_head == b.journal_head &&
        a.descriptor == b.descriptor && a.commit == b.commit && a.end == b.end && a.payloads == b.payloads &&
        a.targets == b.targets && a.count == b.count && a.sequence == b.sequence && a.ready == b.ready;
}
inline bool same_directory(const Ext4OrphanDirectoryReferenceProof &a, const Ext4OrphanDirectoryReferenceProof &b)
{
    return a.filesystem_uuid == b.filesystem_uuid && a.observed_head == b.observed_head &&
        a.directory_inodes == b.directory_inodes && a.directory_blocks == b.directory_blocks &&
        a.live_entries == b.live_entries && a.valid == b.valid;
}
} // namespace ok::fs::reclaim_history
