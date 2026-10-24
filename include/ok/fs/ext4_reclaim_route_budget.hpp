#pragma once

#include "ok/fs/ext4_reclaim_history.hpp"
#include <type_traits>

namespace ok::fs
{
template <usize, usize> class Ext4OwnedTransaction;

enum class Ext4ReclaimRoutePhase : u8 { unused, opening, refined, handoff, closed, failed };
enum class Ext4ReclaimRouteOperation : u8 { read, write, flush, none };

// Public copies carry no authority and cannot admit a writer phase.
struct Ext4ReclaimRouteCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    usize remaining{};
    u32 projected_sequence_end{};
};
struct Ext4ReclaimRouteBudgetReport
{
    Ext4ReclaimRoutePhase phase{Ext4ReclaimRoutePhase::unused};
    Ext4ReclaimRouteCapacity capacity{};
    std::array<block::SessionCounter, 3> attempts{}, completions{};
    block::SessionReport observed{};
    bool attempted{}, refined{}, dispatch_pending{};
};

// The exact original Session and its service outlive this serialized guard.
// Pair each successful begin with one actual Session operation and one finish.
template <typename Session> class Ext4ReclaimRouteBudget final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Phase = Ext4ReclaimRoutePhase;
    using Operation = Ext4ReclaimRouteOperation;
  public:
    Ext4ReclaimRouteBudget() = default;
    ~Ext4ReclaimRouteBudget()
    {
        if (control_.operation != Operation::none || sealed_.operation != Operation::none)
            static_cast<void>(anchor_.abort(Status::interrupted("reclaim route unfinished dispatch")));
    }
    Ext4ReclaimRouteBudget(const Ext4ReclaimRouteBudget &) = delete;
    Ext4ReclaimRouteBudget &operator=(const Ext4ReclaimRouteBudget &) = delete;
    Ext4ReclaimRouteBudget(Ext4ReclaimRouteBudget &&) = delete;
    Ext4ReclaimRouteBudget &operator=(Ext4ReclaimRouteBudget &&) = delete;

    template <typename Counter = block::SessionCounter> requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Result<Ext4ReclaimRouteCapacity> capacity(
        const block::SessionReport &initial, usize remaining, u32 sequence)
    {
        if (remaining > 7) return Status::unsupported("reclaim route remaining-member capacity");
        if (!stable(initial)) return Status::fault("reclaim route capacity requires stable Session");
        const u32 delta = 2 + u32(3 * remaining);
        if (sequence > ~u32{0} - delta)
            return Status::overflow("reclaim route projected sequence exhausted");
        const Ext4ReclaimRouteCapacity result{read_caps_[remaining], 6 + 15 * remaining,
                                              5 + 8 * remaining, remaining, sequence + delta};
        const auto attempts = attempted_counts(initial), completions = completed_counts(initial);
        const std::array<block::SessionCounter, 3> caps{result.reads, result.writes, result.flushes};
        constexpr u64 limit = u64(Counter(~Counter{0}));
        for (usize index = 0; index < caps.size(); ++index)
            if (u64(caps[index]) > limit || u64(attempts[index]) > limit - u64(caps[index]) ||
                u64(completions[index]) > limit - u64(caps[index]))
                return Status::overflow("reclaim route whole attempt/completion capacity exhausted");
        return result;
    }

    [[nodiscard]] Status reserve_opening(Session &session)
    {
        if (control_.operation != Operation::none || sealed_.operation != Operation::none)
            return reject(Status::fault("reclaim route reservation reentered dispatch"));
        if (attempted_) return Status::already_exists("reclaim route reservation is one-shot");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return status;
        original_ = &session;
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        // Reserve all maximum counters before classifier READs; no WRITE grant.
        const auto available = capacity(actual.value(), 7, 0);
        if (!available) return refuse(available.status());
        const auto geometry = session.geometry();
        if (geometry.block_size != 512)
            return refuse(Status::unsupported("reclaim route requires 512-byte sectors"));
        if (!geometry.block_count || !geometry.block_size)
            return reject(Status::fault("reclaim route original Session geometry"));
        const auto bounds = session.bounds();
        if (!valid_bounds(bounds, geometry))
            return reject(Status::fault("reclaim route original Session volume bounds"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = observed_ = actual.value();
        geometry_ = geometry_seal_ = geometry;
        bounds_ = bounds_seal_ = bounds;
        control_.phase = Phase::opening;
        sync();
        return {};
    }

    [[nodiscard]] Status refine(Session &session, usize remaining, u32 active_sequence)
    {
        if (control_.operation != Operation::none)
            return reject(Status::fault("reclaim route refinement reentered dispatch"));
        if (refine_attempted_) return Status::already_exists("reclaim route refinement is one-shot");
        if (auto status = idle_exact(session); !status) return status;
        if (control_.phase != Phase::opening)
            return reject(Status::fault("reclaim route refinement outside opening"));
        refine_attempted_ = true;
        if (!active_sequence)
            return refuse(Status::invalid_argument("reclaim route active sequence must be nonzero"));
        const auto narrowed = capacity(initial_, remaining, active_sequence);
        if (!narrowed) return refuse(narrowed.status());
        if (control_.attempts[0] > narrowed.value().reads ||
            control_.completions[0] > narrowed.value().reads)
            return refuse(Status::overflow("reclaim route opening reads exceed refined capacity"));
        control_.remaining = remaining;
        control_.sequence = active_sequence;
        control_.projected_sequence_end = narrowed.value().projected_sequence_end;
        control_.phase = Phase::refined;
        sync();
        return {};
    }

    [[nodiscard]] Status begin_read(Session &session, u64 first_sector, usize transfer_bytes = 1024)
    { return begin(session, Operation::read, first_sector, transfer_bytes); }
    [[nodiscard]] Status finish_read(Session &session, Status transport)
    { return finish(session, Operation::read, transport); }

    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation == Operation::none)
            return reject(Status::fault("reclaim route callback retained authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_))
        {
            // Keep the independent selected dispatch intact until finish
            // accounts the failed Session attempt. Quarantine prevents any
            // subsequent transport and the Session cannot credit completion.
            const auto status = Status::fault("reclaim route callback original volume changed");
            static_cast<void>(anchor_.abort(status));
            return status;
        }
        auto expected = flight_;
        advance_attempt(expected, control_.operation);
        expected.in_flight = true;
        if (control_.operation == Operation::write) expected.pending_writes = true;
        return reclaim_history::same_session(actual.value(), expected) ? Status{} :
            reject(Status::fault("reclaim route callback Session prefix changed"));
    }

    [[nodiscard]] Status close(Session &session)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (progress_.pending_writes)
            return reject(Status::busy("reclaim route close has unresolved writes"));
        control_.phase = Phase::closed;
        control_.writer = false;
        sync();
        return {};
    }

    [[nodiscard]] Ext4ReclaimRouteBudgetReport report() const
    {
        return {control_.phase, limits(), control_.attempts, control_.completions, observed_,
                attempted_, refine_attempted_, control_.operation != Operation::none};
    }

  private:
    // Only the continuing Owner records this identity before classifier I/O.
    // A public reserve_opening call deliberately cannot acquire this attachment.
    [[nodiscard]] Status reserve_owned_opening(Session &session, const void *owner)
    {
        if (!owner || owner_ || owner_seal_)
            return reject(Status::fault("reclaim route original Owner reservation differs"));
        if (auto status = reserve_opening(session); !status) return status;
        owner_ = owner_seal_ = owner;
        return {};
    }
    // Detach the opening view at quiescence without closing/replacing the budget.
    // This is accounting handoff, not admission of canonical publication or I/O.
    [[nodiscard]] Status quiescent_handoff(Session &session, const void *owner)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (!owner || owner != owner_ || owner_ != owner_seal_ || handoff_attempted_ ||
            control_.phase != Phase::refined || control_.writer || progress_.pending_writes)
            return reject(Status::fault("reclaim route same-Owner handoff unavailable"));
        handoff_attempted_ = true;
        control_.phase = Phase::handoff;
        sync();
        return {};
    }
    // The Owner may account a completed, independently guarded read-only
    // subroute. This never grants WRITE/FLUSH authority and must be called
    // before the Owner adopts the subroute's Session progress shadow.
    [[nodiscard]] Status adopt_verified_reads(Session &session,
                                               const block::SessionReport &before, block::SessionCounter count)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != Operation::none ||
            !reclaim_history::same_session(before, progress_))
            return reject(Status::fault("reclaim route guarded subroute origin differs"));
        const auto bound = limits().reads;
        if (count > bound - control_.attempts[0] || count > bound - control_.completions[0] ||
            count > ~block::SessionCounter{0} - progress_.reads || count > ~block::SessionCounter{0} - progress_.completed_reads)
            return reject(Status::overflow("reclaim route guarded subroute read credit exhausted"));
        auto expected = progress_;
        expected.reads += count; expected.completed_reads += count;
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("reclaim route guarded subroute Session prefix differs"));
        control_.attempts[0] += count; control_.completions[0] += count;
        progress_ = progress_seal_ = actual.value();
        sync();
        return {};
    }

    struct Control
    {
        Phase phase{Phase::unused};
        Operation operation{Operation::none};
        std::array<block::SessionCounter, 3> attempts{}, completions{};
        usize remaining{}, transfer_bytes{};
        u64 first_sector{};
        u32 sequence{}, projected_sequence_end{};
        bool writer{};
    };
    // Full leaf transport route, independently derived with K=2327,
    // H=2335 and O=10163275. Classifier5 + binding4685 + result(2347 or
    // 6989) + composed(1+6981*n+O+H) + physical C1(6981*n) + canonical
    // misses(6*(n+1)) + whole comparisons((7+6*n)*H) + ordinary inspect O
    // + mounted view294 + stored metadata2326 + final journal1.
    // Source-label lookup and cache hits charge no transport READs.
    static constexpr std::array<block::SessionCounter, 8> read_caps_{
        20354895, 20387515, 20415493, 20443471, 20471449, 20499427, 20527405, 20555383};

    static bool stable(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    static std::array<block::SessionCounter, 3> attempted_counts(const block::SessionReport &value)
    { return {value.reads, value.writes, value.flushes}; }
    static std::array<block::SessionCounter, 3> completed_counts(const block::SessionReport &value)
    { return {value.completed_reads, value.completed_writes, value.completed_flushes}; }
    static bool same_geometry(const driver::BlockGeometry &a, const driver::BlockGeometry &b)
    { return a.block_count == b.block_count && a.block_size == b.block_size && a.writable == b.writable; }
    static bool same_bounds(const block::SessionBounds &a, const block::SessionBounds &b)
    { return a.first == b.first && a.count == b.count; }
    static bool valid_bounds(const block::SessionBounds &bounds, const driver::BlockGeometry &geometry)
    { return bounds.count && bounds.count == geometry.block_count && bounds.first <= ~u64{0} - bounds.count; }
    static bool same_control(const Control &a, const Control &b)
    {
        return a.phase == b.phase && a.operation == b.operation && a.attempts == b.attempts &&
            a.completions == b.completions && a.remaining == b.remaining &&
            a.transfer_bytes == b.transfer_bytes && a.first_sector == b.first_sector &&
            a.sequence == b.sequence && a.projected_sequence_end == b.projected_sequence_end &&
            a.writer == b.writer;
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Ext4ReclaimRouteCapacity limits() const
    {
        if (control_.phase == Phase::opening) return {read_caps_[7], 111, 61, 7, 0};
        if (control_.remaining > 7) return {};
        return {read_caps_[control_.remaining], 6 + 15 * control_.remaining,
                5 + 8 * control_.remaining, control_.remaining, control_.projected_sequence_end};
    }
    bool retained_exact() const
    {
        if (!attempted_ || !original_ || !anchor_.exact(original_) ||
            (control_.phase != Phase::opening && control_.phase != Phase::refined && control_.phase != Phase::handoff) ||
            owner_ != owner_seal_ ||
            (control_.writer && control_.phase != Phase::handoff) ||
            (handoff_attempted_ != (control_.phase == Phase::handoff)) ||
            (handoff_attempted_ && !owner_) ||
            !same_control(control_, sealed_) ||
            (control_.operation != Operation::none && !same_control(control_, flight_control_)) ||
            !reclaim_history::same_session(initial_, initial_seal_) ||
            !reclaim_history::same_session(progress_, progress_seal_) ||
            !reclaim_history::same_session(flight_, flight_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            !valid_bounds(bounds_, geometry_) || !stable(initial_)) return false;
        if (control_.phase == Phase::opening &&
            (refine_attempted_ || control_.writer || control_.remaining || control_.sequence ||
             control_.projected_sequence_end)) return false;
        const auto caps = control_.phase == Phase::opening ? capacity(initial_, 7, 0) :
            capacity(initial_, control_.remaining, control_.sequence);
        if (!caps || (control_.phase != Phase::opening &&
            (!refine_attempted_ || caps.value().projected_sequence_end != control_.projected_sequence_end))) return false;
        const auto bound = limits();
        const std::array<block::SessionCounter, 3> bounds{bound.reads, bound.writes, bound.flushes};
        const auto initial_attempts = attempted_counts(initial_), initial_completed = completed_counts(initial_);
        const auto progress_attempts = attempted_counts(progress_), progress_completed = completed_counts(progress_);
        for (usize index = 0; index < bounds.size(); ++index)
            if (control_.attempts[index] > bounds[index] || control_.completions[index] != control_.attempts[index] ||
                initial_attempts[index] > ~block::SessionCounter{0} - control_.attempts[index] ||
                initial_completed[index] > ~block::SessionCounter{0} - control_.completions[index] ||
                progress_attempts[index] != initial_attempts[index] + control_.attempts[index] ||
                progress_completed[index] != initial_completed[index] + control_.completions[index]) return false;
        if (progress_.identity != initial_.identity ||
            progress_.registration_generation != initial_.registration_generation ||
            progress_.session_serial != initial_.session_serial || progress_.state != block::DeviceState::owned ||
            progress_.first_error != StatusCode::ok || progress_.in_flight) return false;
        return control_.writer || (!progress_.pending_writes && !control_.attempts[1] && !control_.attempts[2]);
    }

    [[nodiscard]] Status refuse(Status status)
    {
        attempted_ = true;
        control_.phase = Phase::failed;
        control_.writer = false;
        sync();
        return status;
    }
    [[nodiscard]] Status reject(Status status)
    {
        static_cast<void>(anchor_.abort(status));
        if (anchor_.exact(original_))
        {
            const auto actual = original_->report();
            if (actual) observed_ = actual.value();
        }
        return refuse(status);
    }
    [[nodiscard]] Status idle_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != Operation::none)
            return reject(Status::fault("reclaim route original quiescent authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        if (!reclaim_history::same_session(actual.value(), progress_) ||
            !same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_))
            return reject(Status::fault("reclaim route quiescent Session progress changed"));
        return {};
    }

    // Accounting is not admission. Only a future exact Owner phase may call this.
    [[nodiscard]] Status enter_writer_phase(Session &session, const void *owner)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (control_.phase != Phase::handoff || !handoff_attempted_ || !owner ||
            owner != owner_ || owner_ != owner_seal_ || control_.writer || !geometry_.writable)
            return reject(Status::denied("reclaim route private writer phase unavailable"));
        control_.writer = true;
        sync();
        return {};
    }
    [[nodiscard]] Status begin_write(Session &session, u64 first_sector, usize transfer_bytes = 1024)
    { return begin(session, Operation::write, first_sector, transfer_bytes); }
    [[nodiscard]] Status finish_write(Session &session, Status transport)
    { return finish(session, Operation::write, transport); }
    [[nodiscard]] Status begin_flush(Session &session)
    { return begin(session, Operation::flush, 0, 0); }
    [[nodiscard]] Status finish_flush(Session &session, Status transport)
    { return finish(session, Operation::flush, transport); }

    [[nodiscard]] Status begin(Session &session, Operation operation, u64 first_sector, usize transfer_bytes)
    {
        if (auto status = idle_exact(session); !status) return status;
        if (operation != Operation::read && !control_.writer)
            return Status::denied("reclaim route WRITE/FLUSH require private admitted phase");
        if (operation == Operation::read && progress_.pending_writes)
            return Status::busy("reclaim route READ before successful FLUSH");
        if (operation != Operation::flush)
        {
            const auto transfer = session.transfer_limit();
            if (!transfer) return reject(transfer.status());
            if (first_sector % 2 || transfer_bytes != 1024 ||
                transfer_bytes % geometry_.block_size || transfer_bytes > transfer.value() ||
                first_sector >= geometry_.block_count ||
                transfer_bytes / geometry_.block_size > geometry_.block_count - first_sector)
                return Status::invalid_argument("reclaim route selected transfer bounds");
        }
        const usize index = usize(operation);
        if (index >= 3) return reject(Status::fault("reclaim route selected operation"));
        const auto bound = limits();
        const std::array<block::SessionCounter, 3> caps{bound.reads, bound.writes, bound.flushes};
        const auto attempts = attempted_counts(progress_), completions = completed_counts(progress_);
        if (control_.attempts[index] >= caps[index] || control_.completions[index] >= caps[index] ||
            attempts[index] == ~block::SessionCounter{0} || completions[index] == ~block::SessionCounter{0})
            return Status::overflow("reclaim route selected dispatch credit exhausted");
        flight_ = flight_seal_ = progress_;
        control_.operation = operation;
        control_.first_sector = first_sector;
        control_.transfer_bytes = transfer_bytes;
        sync();
        flight_control_ = control_;
        return {};
    }
    static void advance_attempt(block::SessionReport &report, Operation operation)
    {
        if (operation == Operation::read) ++report.reads;
        if (operation == Operation::write) ++report.writes;
        if (operation == Operation::flush) ++report.flushes;
    }
    static void advance_completion(block::SessionReport &report, Operation operation)
    {
        if (operation == Operation::read) ++report.completed_reads;
        if (operation == Operation::write) ++report.completed_writes;
        if (operation == Operation::flush) ++report.completed_flushes;
    }
    [[nodiscard]] Status finish(Session &session, Operation operation, Status transport)
    {
        if (anchor_.exact(&session) && original_ == &session)
        {
            const auto actual = session.report();
            if (actual) observed_ = actual.value();
        }
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session ||
            control_.operation != operation || operation == Operation::none)
            return reject(Status::fault("reclaim route post-dispatch retained authority changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        const bool exact_volume = same_geometry(session.geometry(), geometry_) && same_bounds(session.bounds(), bounds_);
        if (!exact_volume && transport)
            return reject(Status::fault("reclaim route post-dispatch original volume changed"));
        auto expected = flight_;
        advance_attempt(expected, operation);
        if (operation == Operation::write) expected.pending_writes = true;
        if (transport)
        {
            advance_completion(expected, operation);
            if (operation == Operation::flush) expected.pending_writes = false;
        }
        else
        {
            expected.state = block::DeviceState::quarantined;
            // An in-flight abort may retain an earlier error than transport.
            expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok)
                return reject(Status::fault("reclaim route transport failure lacks quarantine error"));
        }
        if (!reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("reclaim route transport attempt/completion prefix differs"));
        const usize index = usize(operation);
        ++control_.attempts[index];
        if (transport) ++control_.completions[index];
        progress_ = progress_seal_ = actual.value();
        control_.operation = Operation::none;
        control_.first_sector = 0;
        control_.transfer_bytes = 0;
        sync();
        if (!exact_volume)
            return reject(Status::fault("reclaim route post-dispatch original volume changed"));
        return transport ? Status{} : reject(transport);
    }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Control control_{}, sealed_{}, flight_control_{};
    const void *owner_{}, *owner_seal_{};
    bool attempted_{}, refine_attempted_{}, handoff_attempted_{};
};
} // namespace ok::fs
