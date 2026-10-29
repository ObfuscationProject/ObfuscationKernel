#pragma once

// ext4 mount module: mounted typed cursors and budgets, the owned transaction, empty-orphan
// recovery and the mounted metadata provider.

#include "ok/fs/ext4/journal.hpp"
#include "ok/fs/ext4/alloc.hpp"
#include "ok/fs/ext4/dir.hpp"
#include "ok/fs/ext4/txn.hpp"
#include "ok/fs/ext4/recovery.hpp"
#include "ok/block/session_counters.hpp"
#include "ok/block/sessions.hpp"
#include "ok/fs/ext4_directory_create_sdk_profile.hpp"
#include "ok/block/guarded_reads.hpp"
#include "ok/fs/mount_retention.hpp"
#include "ok/fs/mode_policy.hpp"
#include "ok/arch/realtime.hpp"
#include <array>
#include <type_traits>
#include <concepts>
#include <new>
#include <optional>
#include <utility>

namespace ok::fs
{
enum class Ext4MountedCommonFamily : u8
{ ordinary, legacy, empty_orphan, namespace_recovery, reclaim, typed_zero, typed_allocated, directory_create, regular_move };

struct Ext4MountedCommonCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 sequence_delta{};
    u8 locator_reads{};
    friend bool operator==(const Ext4MountedCommonCapacity &, const Ext4MountedCommonCapacity &) = default;
};

// Fixed G4/F32768 arithmetic only. Neither operands nor results authenticate a
// family, a Session endpoint, a retained opening or permission to issue I/O.
class Ext4MountedCommonOpeningBudget final
{
  public:
    using Counter = block::SessionCounter;
    // Attempted/completed READ, WRITE and FLUSH, in that order.
    using Counts = std::array<Counter, 6>;
    using Family = Ext4MountedCommonFamily;
    using Capacity = Ext4MountedCommonCapacity;

    [[nodiscard]] static Result<Capacity> route(Family family)
    {
        const auto index = static_cast<usize>(family);
        if (index >= routes_.size()) return Status::invalid_argument("common opening budget family");
        return routes_[index];
    }

    [[nodiscard]] static constexpr Capacity whole_union()
    {
        Capacity out{};
        for (const auto &row : routes_)
        {
            if (row.reads > out.reads) out.reads = row.reads;
            if (row.writes > out.writes) out.writes = row.writes;
            if (row.flushes > out.flushes) out.flushes = row.flushes;
        }
        // Sequence and locator are learned from the selected real family.
        return out;
    }

    template <block::SessionCounterPolicy Policy = Counter>
    [[nodiscard]] static constexpr Status reserve(const Counts &baseline)
    {
        const auto capacities = fields(whole_union());
        for (usize index = 0; index < baseline.size(); ++index)
            if (!block::session_counter_fits<Policy>(baseline[index], capacities[index]))
                return Status::overflow("common opening six-counter union exhausted");
        return {};
    }

    template <block::SessionCounterPolicy Policy = Counter>
    [[nodiscard]] static Result<Capacity> narrow(const Counts &baseline,
        const Counts &endpoint, Family family, u8 classifier_reads)
    {
        if (auto status = reserve<Policy>(baseline); !status) return status;
        const auto selected = route(family);
        if (!selected) return selected.status();
        const auto capacity = selected.value();
        if (classifier_reads != capacity.locator_reads)
            return Status::invalid_argument("common opening locator count differs from family");
        for (usize index = 0; index < baseline.size(); ++index)
        {
            const Counter spent = index < 2 ? classifier_reads : 0;
            if (endpoint[index] < baseline[index] || endpoint[index] - baseline[index] != spent)
                return Status::invalid_argument("common opening classifier counter delta");
        }
        if (baseline[0] != baseline[1] || baseline[2] != baseline[3] || baseline[4] != baseline[5])
            return Status::invalid_argument("common opening baseline requires completed parity");
        if (classifier_reads > capacity.reads)
            return Status::overflow("common opening locator exceeds selected route");
        auto remaining = capacity;
        remaining.reads -= classifier_reads;
        const auto capacities = fields(remaining);
        for (usize index = 0; index < endpoint.size(); ++index)
            if (!block::session_counter_fits<Policy>(endpoint[index], capacities[index]))
                return Status::overflow("common opening remaining route exhausted");
        return remaining;
    }

  private:
    [[nodiscard]] static constexpr Counts fields(const Capacity &capacity)
    {
        return {capacity.reads, capacity.reads, capacity.writes, capacity.writes,
                capacity.flushes, capacity.flushes};
    }

    // Source-derived upper bounds, including the locator exactly once and the
    // complete terminal/root continuation. Their derivation is pinned in
    // docs/storage/common-opening-budget.md; tests use that independent oracle.
    inline static constexpr std::array<Capacity, 9> routes_{{
        {58'811'342'498ULL, 8, 6, 2, 4},
        {23'524'424'765ULL, 7, 5, 2, 5},
        {11'762'325'977ULL, 120, 64, 24, 4},
        {11'762'191'265ULL, 21, 11, 5, 5},
        {23'524'315'345ULL, 111, 61, 23, 5},
        {2'917'720'020ULL, 5, 3, 1, 5},
        {2'917'720'035ULL, 5, 3, 1, 5},
        {23'524'132'523ULL, 9, 5, 2, 5},
        // 5 locator + 143 move + 1 final head + inherited terminal/root
        // continuation (11,762,118,846). No orphan chain follows this family.
        {11'762'118'995ULL, 6, 3, 2, 5},
    }};
};

enum class Ext4MountedTypedRoute : u8
{ zero_fresh, zero_recovery, zero_noop, allocated_fresh, allocated_recovery };
enum class Ext4MountedTypedOrigin : u8 { mutation, startup };

struct Ext4MountedTypedBounds
{
    u64 decode{}, view_open{}, stored_metadata{}, completion{}, strict_empty{};
    u64 mutation_prefix{}, mutation_suffix{}, startup_suffix{}, directory_prefix_sum{};
    friend bool operator==(const Ext4MountedTypedBounds &, const Ext4MountedTypedBounds &) = default;
};
struct Ext4MountedTypedCapacity
{
    u64 classifier_reads{}, owner_prefix_reads{}, child_reads{}, suffix_reads{};
    u64 reads{}, writes{}, flushes{};
    u32 groups{}, sequence_delta{};
    u64 filesystem_blocks{};
    Ext4MountedTypedRoute route{};
    Ext4MountedTypedOrigin origin{};
    friend bool operator==(const Ext4MountedTypedCapacity &, const Ext4MountedTypedCapacity &) = default;
};

// Inert reservation facts. They grant no role, Session, child or writer authority.
class Ext4MountedTypedBudget final
{
  public:
    [[nodiscard]] static Result<Ext4MountedTypedBounds> bounds(u32 groups, u64 filesystem_blocks)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !filesystem_blocks)
            return Status::invalid_argument("mounted typed budget geometry");
        Ext4MountedTypedBounds out{};
        u64 scaled{}, coefficient{}, terms{};
        if (!multiply(4, groups, scaled) || !add(2, scaled, out.decode) ||
            !multiply(2, out.decode, scaled) || !add(1026, scaled, out.view_open) ||
            !multiply(129, out.decode, scaled) || !add(groups, scaled, out.stored_metadata))
            return Status::overflow("mounted typed decoder budget overflow");
        out.directory_prefix_sum = filesystem_blocks < 129 * u64{1024} ? filesystem_blocks : 129 * u64{1024};
        // Each prefix includes its first attempted failing mapped data block.
        // Its aggregate bound requires the actual continuing child ownership proof.
        if (!multiply(168, out.decode, scaled) || !add(86017, scaled, coefficient) ||
            !multiply(coefficient, out.directory_prefix_sum, terms) ||
            !multiply(515, out.decode, scaled) || !add(groups, scaled, out.completion) ||
            !add(out.completion, terms, out.completion) || !multiply(2, groups, scaled) ||
            !add(7, scaled, out.strict_empty) || !add(out.strict_empty, out.view_open, out.strict_empty) ||
            !add(out.strict_empty, out.completion, out.strict_empty) ||
            !multiply(2, out.decode, scaled) || !add(scaled, 1, out.mutation_prefix) ||
            !add(out.strict_empty, out.view_open, terms) || !add(terms, out.stored_metadata, terms) ||
            !add(terms, scaled, out.mutation_suffix) || !add(out.mutation_suffix, 2, out.mutation_suffix) ||
            !add(out.mutation_suffix, out.decode, out.startup_suffix))
            return Status::overflow("mounted typed suffix budget overflow");
        return out;
    }

    template <typename Counter = block::SessionCounter>
        requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Status counters(const block::SessionReport &initial,
                                         const Ext4MountedTypedCapacity &capacity)
    {
        constexpr u64 limit = u64(Counter(~Counter{0}));
        const auto fits = [limit](block::SessionCounter used, u64 reserved) {
            return u64(used) <= limit && reserved <= limit - u64(used);
        };
        if (!fits(initial.reads, capacity.reads) || !fits(initial.completed_reads, capacity.reads) ||
            !fits(initial.writes, capacity.writes) || !fits(initial.completed_writes, capacity.writes) ||
            !fits(initial.flushes, capacity.flushes) || !fits(initial.completed_flushes, capacity.flushes))
            return Status::overflow("mounted typed whole attempted/completed counters exhausted");
        return {};
    }

    template <typename Counter = block::SessionCounter>
        requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Result<Ext4MountedTypedCapacity> capacity(
        const block::SessionReport &initial, u32 groups, u64 filesystem_blocks,
        Ext4MountedTypedOrigin origin, Ext4MountedTypedRoute route)
    {
        if ((origin != Ext4MountedTypedOrigin::mutation && origin != Ext4MountedTypedOrigin::startup) ||
            (route != Ext4MountedTypedRoute::zero_fresh && route != Ext4MountedTypedRoute::zero_recovery &&
             route != Ext4MountedTypedRoute::zero_noop && route != Ext4MountedTypedRoute::allocated_fresh &&
             route != Ext4MountedTypedRoute::allocated_recovery))
            return Status::invalid_argument("mounted typed budget route");
        if (origin == Ext4MountedTypedOrigin::startup &&
            (groups != 4 || filesystem_blocks != 32768 ||
             (route != Ext4MountedTypedRoute::zero_recovery && route != Ext4MountedTypedRoute::allocated_recovery)))
            return Status::unsupported("mounted typed startup fixed locator profile");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("mounted typed budget requires a stable original Session");
        const auto computed = bounds(groups, filesystem_blocks);
        if (!computed) return computed.status();
        const auto &bound = computed.value();
        Ext4MountedTypedCapacity out{};
        out.groups = groups; out.filesystem_blocks = filesystem_blocks; out.origin = origin; out.route = route;
        out.classifier_reads = origin == Ext4MountedTypedOrigin::startup ? 5 : bound.mutation_prefix;
        // The original Owner rereads every resident canonical slot before
        // handing off the child. Reserve the same nine-slot production profile.
        out.owner_prefix_reads = ext4_directory_create_owner_buffers;
        out.suffix_reads = origin == Ext4MountedTypedOrigin::startup ? bound.startup_suffix : bound.mutation_suffix;
        u64 base{}, coefficient{}, scaled{};
        switch (route)
        {
        case Ext4MountedTypedRoute::zero_fresh:
            base = 5849; coefficient = 2124; out.writes = 13; out.flushes = 6; out.sequence_delta = 2; break;
        case Ext4MountedTypedRoute::zero_recovery:
            base = 4351; coefficient = 1593; out.writes = 5; out.flushes = 3; out.sequence_delta = 1; break;
        case Ext4MountedTypedRoute::zero_noop:
            base = 1446; coefficient = 531; out.suffix_reads = 1; break;
        case Ext4MountedTypedRoute::allocated_fresh:
            base = 5884; coefficient = 2125; out.writes = 14; out.flushes = 7; out.sequence_delta = 2; break;
        case Ext4MountedTypedRoute::allocated_recovery:
            base = 4366; coefficient = 1593; out.writes = 5; out.flushes = 3; out.sequence_delta = 1; break;
        }
        if (!multiply(coefficient, groups, scaled) || !add(base, scaled, out.child_reads) ||
            !add(out.classifier_reads, out.owner_prefix_reads, out.reads) ||
            !add(out.reads, out.child_reads, out.reads) || !add(out.reads, out.suffix_reads, out.reads))
            return Status::overflow("mounted typed whole route budget overflow");
        if (auto status = counters<Counter>(initial, out); !status) return status;
        return out;
    }

    // Startup learns sequence at head READ4. This pure refinement follows the
    // counter reservation; a caller must authenticate it before writer admission.
    [[nodiscard]] static Result<u32> final_sequence(Ext4MountedTypedRoute route, u32 sequence)
    {
        if (!sequence) return Status::invalid_argument("mounted typed zero journal sequence");
        switch (route)
        {
        case Ext4MountedTypedRoute::zero_noop: return sequence;
        case Ext4MountedTypedRoute::zero_fresh:
        case Ext4MountedTypedRoute::allocated_fresh:
            if (sequence > 0xfffffffcU) return Status::overflow("mounted typed fresh sequence exhausted");
            return sequence + 2;
        case Ext4MountedTypedRoute::zero_recovery:
        case Ext4MountedTypedRoute::allocated_recovery:
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("mounted typed recovery sequence exhausted");
            return sequence + 1;
        }
        return Status::invalid_argument("mounted typed sequence route");
    }
  private:
    [[nodiscard]] static bool add(u64 first, u64 second, u64 &out)
    {
        if (second > ~u64{0} - first) return false;
        out = first + second; return true;
    }
    [[nodiscard]] static bool multiply(u64 first, u64 second, u64 &out)
    {
        if (first && second > ~u64{0} / first) return false;
        out = first * second; return true;
    }
};

struct Ext4DirectReadHooks final
{
    template <typename Session>
    [[nodiscard]] Status begin_read(Session &, u64, usize) { return {}; }
    template <typename Session>
    [[nodiscard]] Status finish_read(Session &, Status) { return {}; }
};

// The Session, shadow and authority outlive this serialized, call-only scope.
// A successful finish verifies physical progress; adoption follows the complete
// dispatcher, including any external finish hook.
template <typename Session, typename Authority, bool AllowCompletedPendingWrites = false>
    requires requires(Authority &authority) { { authority() } -> std::same_as<Status>; }
class Ext4DirectReadScope final
{
    enum class Phase : u8 { unused, prepared, pending, verified, adopted };
  public:
    Ext4DirectReadScope(Session &session, block::SessionReport &shadow, Authority &authority,
                        u64 first, usize bytes)
        : original_(&session), shadow_(&shadow), shadow_identity_(&shadow), authority_(authority),
          first_(first), bytes_(bytes)
    { static_cast<void>(anchor_.bind(session)); }
    ~Ext4DirectReadScope()
    {
        if ((begun_ || begun_seal_) && (phase_ != Phase::adopted || phase_seal_ != Phase::adopted))
            static_cast<void>(anchor_.abort(Status::interrupted("direct read scope ended before adoption")));
    }
    Ext4DirectReadScope(const Ext4DirectReadScope &) = delete;
    Ext4DirectReadScope &operator=(const Ext4DirectReadScope &) = delete;
    Ext4DirectReadScope(Ext4DirectReadScope &&) = delete;
    Ext4DirectReadScope &operator=(Ext4DirectReadScope &&) = delete;

    [[nodiscard]] Status prepare()
    {
        if (phase_ != Phase::unused || phase_seal_ != Phase::unused)
            return Status::already_exists("direct read scope is one-shot");
        if (!anchor_.exact(original_) || shadow_ != shadow_identity_)
            return reject(Status::fault("direct read original attachment differs"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        if (!healthy(actual.value()) || !reclaim_history::same_session(actual.value(), *shadow_))
            return reject(Status::fault("direct read initial Session or Owner shadow differs"));
        before_ = before_seal_ = actual.value();
        geometry_ = geometry_seal_ = original_->geometry();
        if (geometry_.block_size != 512 || !geometry_.block_count)
            return Status::unsupported("direct ext4 read requires 512-byte sectors");
        const auto limit = original_->transfer_limit();
        if (!limit) return limit.status();
        limit_ = limit_seal_ = limit.value();
        if (auto status = authority_(); !status) return status;
        phase_ = phase_seal_ = Phase::prepared;
        return {};
    }

    [[nodiscard]] Status prepared_exact(Session &session)
    {
        if (phase_ != Phase::prepared) return reject(Status::fault("direct read prepared phase differs"));
        return observe(session, before_);
    }
    [[nodiscard]] Status begin_read(Session &session, u64 first, usize bytes)
    {
        if (auto status = prepared_exact(session); !status) return status;
        if (first != first_ || bytes != bytes_)
            return reject(Status::fault("direct read selected transfer changed"));
        if (before_.reads == ~block::SessionCounter{0} || before_.completed_reads == ~block::SessionCounter{0})
            return Status::overflow("direct read single attempt or completion exhausted");
        if (!bytes || bytes % geometry_.block_size || bytes > limit_ || first >= geometry_.block_count ||
            bytes / geometry_.block_size > geometry_.block_count - first)
            return Status::invalid_argument("direct read transfer outside original bounds");
        begun_ = begun_seal_ = true;
        phase_ = phase_seal_ = Phase::pending;
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (phase_ != Phase::pending || !begun_)
            return reject(Status::fault("direct read observation has no pending dispatch"));
        auto expected = before_;
        ++expected.reads;
        expected.in_flight = true;
        return observe(session, expected);
    }
    [[nodiscard]] Status finish_read(Session &session, Status transport)
    {
        // A failed transport never depends on mutable callback authority.
        if (!transport)
        {
            static_cast<void>(anchor_.abort(transport));
            return transport;
        }
        if (phase_ != Phase::pending || !begun_)
            return reject(Status::fault("direct read finish has no pending dispatch"));
        auto expected = completed();
        if (auto status = observe(session, expected); !status) return status;
        phase_ = phase_seal_ = Phase::verified;
        return {};
    }
    [[nodiscard]] Status adopt(Session &session)
    {
        if (phase_ != Phase::verified || !begun_)
            return reject(Status::fault("direct read adoption lacks verified finish"));
        const auto expected = completed();
        if (auto status = observe(session, expected); !status) return status;
        *shadow_ = expected;
        phase_ = phase_seal_ = Phase::adopted;
        return {};
    }
  private:
    [[nodiscard]] static bool healthy(const block::SessionReport &report)
    {
        return report.state == block::DeviceState::owned && report.first_error == StatusCode::ok &&
            report.identity && report.registration_generation && report.session_serial &&
            !report.in_flight && (AllowCompletedPendingWrites || !report.pending_writes) && report.reads == report.completed_reads &&
            report.writes == report.completed_writes && report.flushes == report.completed_flushes;
    }
    [[nodiscard]] static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_count == b.block_count && a.block_size == b.block_size && a.writable == b.writable; }
    [[nodiscard]] block::SessionReport completed() const
    { auto result = before_; ++result.reads; ++result.completed_reads; return result; }
    [[nodiscard]] Status reject(Status status)
    { static_cast<void>(anchor_.abort(status)); return status; }
    [[nodiscard]] Status observe(Session &session, const block::SessionReport &expected)
    {
        if (phase_ != phase_seal_ || begun_ != begun_seal_ || !anchor_.exact(&session) || original_ != &session ||
            shadow_ != shadow_identity_ || !reclaim_history::same_session(before_, before_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || limit_ != limit_seal_ ||
            !reclaim_history::same_session(*shadow_, before_))
            return reject(Status::fault("direct read retained attachment or paused Owner shadow differs"));
        if (!same_geometry(session.geometry(), geometry_))
            return reject(Status::fault("direct read original geometry changed"));
        const auto actual = session.report();
        if (!actual) return reject(actual.status());
        if (!reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("direct read exact Session prefix differs"));
        const auto limit = session.transfer_limit();
        if (!limit) return reject(limit.status());
        if (limit.value() != limit_) return reject(Status::fault("direct read original transfer limit changed"));
        if (auto status = authority_(); !status) return reject(status);
        return {};
    }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{};
    block::SessionReport *shadow_{}, *shadow_identity_{};
    Authority &authority_;
    const u64 first_;
    const usize bytes_;
    block::SessionReport before_{}, before_seal_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    usize limit_{}, limit_seal_{};
    Phase phase_{Phase::unused}, phase_seal_{Phase::unused};
    bool begun_{}, begun_seal_{};
};

template <typename Scope, typename Hook, typename Session>
    requires block::ReadDispatchGuard<Scope, Session> && block::ReadDispatchGuard<Hook, Session>
struct Ext4DirectReadDispatch final
{
    Scope &scope;
    Hook &hook;
    [[nodiscard]] Status begin_read(Session &session, u64 first, usize bytes)
    {
        const auto external = hook.begin_read(session, first, bytes);
        if (!external)
        {
            if (auto status = scope.prepared_exact(session); !status) return status;
            return external;
        }
        return scope.begin_read(session, first, bytes);
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (auto status = scope.in_flight_exact(session); !status) return status;
        if constexpr (requires { { hook.in_flight_exact(session) } -> std::same_as<Status>; })
        {
            const auto external = hook.in_flight_exact(session);
            if (auto status = scope.in_flight_exact(session); !status) return status;
            return external;
        }
        return {};
    }
    [[nodiscard]] Status finish_read(Session &session, Status transport)
    {
        const auto accounted = scope.finish_read(session, transport);
        const auto external = hook.finish_read(session, transport);
        if (!transport) return transport;
        return accounted ? external : accounted;
    }
};

template <typename Session, typename Authority, typename Hook>
    requires block::ReadDispatchGuard<Hook, Session>
[[nodiscard]] Status guarded_ext4_direct_read(Session &session, block::SessionReport &shadow,
    Authority &authority, u64 first, std::span<std::byte> bytes, Hook &hook)
{
    Ext4DirectReadScope<Session, Authority> scope(session, shadow, authority, first, bytes.size());
    if (auto status = scope.prepare(); !status) return status;
    Ext4DirectReadDispatch<decltype(scope), Hook, Session> dispatch{scope, hook};
    if (auto status = block::guarded_read_blocks(session, dispatch, first, bytes); !status) return status;
    return scope.adopt(session);
}

template <typename Session, typename Authority>
[[nodiscard]] Status guarded_ext4_direct_read(Session &session, block::SessionReport &shadow,
    Authority &authority, u64 first, std::span<std::byte> bytes)
{
    Ext4DirectReadHooks hook;
    return guarded_ext4_direct_read(session, shadow, authority, first, bytes, hook);
}

template <usize, usize> class Ext4OwnedTransaction;
template <usize> class Ext4MountedMetadata;

enum class Ext4MountedTypedCursorPhase : u8
{ unused, classifier, refined, paused_child, suffix, closed, refused, failed };
enum class Ext4MountedTypedOpening : u8 { zero_family, allocated_family, noop_only, startup_recovery_family };
enum class Ext4MountedTypedSuffixStage : u8
{ none, inspect_empty, reopen_view, stored_metadata, target_inode, raw_target,
  target_metadata, final_wrapper, startup_root, noop_terminal };
struct Ext4MountedTypedCursorReport {
    Ext4MountedTypedCursorPhase phase{};
    Ext4MountedTypedSuffixStage stage{};
    Ext4MountedTypedCapacity reserved{};
    block::SessionReport observed{};
    u64 classifier_reads{}, suffix_reads{}, stage_reads{}, serial{};
    u32 final_sequence{};
    bool pending{}, receipt_consumed{}, child_bridge_available{};
};

// A private finite protocol leaf. Public observations and construction confer no
// authority. The original service, backing, context and storage remain borrowed.
template <typename Session, typename Authority>
    requires requires(Authority &a, Ext4MountedTypedCursorPhase p, Ext4MountedTypedSuffixStage s) {
        { a.validate(p, s) } -> std::same_as<Status>;
        { a.complete_stage(s) } -> std::same_as<Status>;
    }
class Ext4MountedTypedCursor final {
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
    using NativeSession = Session;
    using Phase = Ext4MountedTypedCursorPhase;
    using Stage = Ext4MountedTypedSuffixStage;
    using Opening = Ext4MountedTypedOpening;
    using Origin = Ext4MountedTypedOrigin;
    using Route = Ext4MountedTypedRoute;
    using OwnerBridgeCheck = Status (*)(const void *, const void *, const void *);
    struct Context final {
        friend class Ext4MountedTypedCursor;
        template <usize, usize> friend class Ext4OwnedTransaction;
        template <usize> friend class Ext4MountedMetadata;
      private:
        template <usize Devices, usize Buffers>
        Context(Session &session, Authority &authority, const Ext4OwnedTransaction<Devices, Buffers> &owner,
                const Ext4MountedMetadata<Devices> &mount, u64 serial, const void *slot = nullptr,
                OwnerBridgeCheck bridge_check = nullptr)
            : session_(session), authority_(authority), owner_(&owner), mount_(&mount), serial_(serial),
              slot_(slot), bridge_check_(bridge_check) {}
        Context(const Context &) = delete; Context &operator=(const Context &) = delete;
        Context(Context &&) = delete; Context &operator=(Context &&) = delete;
        Session &session_; Authority &authority_;
        const void *const owner_; const void *const mount_; const u64 serial_; const void *const slot_;
        const OwnerBridgeCheck bridge_check_;
    };
    struct Receipt final {
        friend class Ext4MountedTypedCursor;
        template <usize, usize> friend class Ext4OwnedTransaction;
        template <usize> friend class Ext4MountedMetadata;
      private:
        Receipt() = default;
        Receipt(const Receipt &) = delete; Receipt &operator=(const Receipt &) = delete;
        Receipt(Receipt &&) = delete; Receipt &operator=(Receipt &&) = delete;
        const Ext4MountedTypedCursor *cursor_{}; const Context *context_{};
        const Session *session_{}; const void *owner_{}; const void *mount_{};
        u64 serial_{}; u32 sequence_{}; block::SessionReport endpoint_{};
        bool minted_{}, consumed_{};
    };
    struct Control final {
        Phase phase{}; Stage stage{};
        u64 classifier_reads{}, suffix_reads{}, stage_reads{}, stage_limit{}, serial{}, counter_limit{};
        u64 sector{}; usize bytes{}; u32 final_sequence{}, stage_index{};
        bool pending{}, dispatch_seen{}, receipt_consumed{};
        friend bool operator==(const Control &, const Control &) = default;
    };
  public:
    explicit Ext4MountedTypedCursor(Session &original) : original_anchor_(original) {}
    ~Ext4MountedTypedCursor() {
        if ((attempted_ || attempted_seal_) && (control_ != sealed_ ||
            (control_.phase != Phase::closed && control_.phase != Phase::refused && control_.phase != Phase::failed)))
            static_cast<void>(original_anchor_.abort(Status::interrupted("mounted typed cursor ended unfinished")));
    }
    Ext4MountedTypedCursor(const Ext4MountedTypedCursor &) = delete;
    Ext4MountedTypedCursor &operator=(const Ext4MountedTypedCursor &) = delete;
    Ext4MountedTypedCursor(Ext4MountedTypedCursor &&) = delete;
    Ext4MountedTypedCursor &operator=(Ext4MountedTypedCursor &&) = delete;
    [[nodiscard]] Ext4MountedTypedCursorReport report() const {
        return {control_.phase, control_.stage, capacity_, observed_, control_.classifier_reads,
                control_.suffix_reads, control_.stage_reads, control_.serial, control_.final_sequence,
                control_.pending, control_.receipt_consumed, bridge_minted_};
    }
  private:
    static bool same_geometry(const driver::BlockGeometry &a, const driver::BlockGeometry &b) {
        return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable;
    }
    static bool same_bounds(const block::SessionBounds &a, const block::SessionBounds &b) {
        return a.first == b.first && a.count == b.count;
    }
    static bool permitted(Origin origin, Opening opening, Route route) {
        if (origin == Origin::startup)
            return opening == Opening::startup_recovery_family &&
                (route == Route::zero_recovery || route == Route::allocated_recovery);
        if (origin != Origin::mutation) return false;
        if (opening == Opening::zero_family)
            return route == Route::zero_fresh || route == Route::zero_recovery || route == Route::zero_noop;
        if (opening == Opening::allocated_family)
            return route == Route::allocated_fresh || route == Route::allocated_recovery;
        return opening == Opening::noop_only && route == Route::zero_noop;
    }
    static bool fits(const block::SessionReport &r, const Ext4MountedTypedCapacity &c, u64 limit) {
        const auto field = [limit](block::SessionCounter used, u64 reserved) { return u64(used) <= limit && reserved <= limit - u64(used); };
        return field(r.reads, c.reads) && field(r.completed_reads, c.reads) &&
            field(r.writes, c.writes) && field(r.completed_writes, c.writes) &&
            field(r.flushes, c.flushes) && field(r.completed_flushes, c.flushes);
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Status reject(Status error) {
        if (error) error = Status::fault("mounted typed cursor rejection requires error");
        static_cast<void>(original_anchor_.abort(error));
        const auto actual = original_anchor_.report();
        if (actual) observed_ = actual.value();
        control_.phase = Phase::failed; control_.pending = false; sync(); context_ = context_seal_ = nullptr;
        return error;
    }
    // The dispatch retains the already-validated actual reference independently
    // of every mutable cursor pointer/anchor. A drifting pointer cannot redirect
    // terminal quarantine to a same-number foreign Session.
    [[nodiscard]] Status reject_original(Session &original, Status error) {
        if (error) error = Status::fault("mounted typed cursor dispatch rejection requires error");
        static_cast<void>(original);
        static_cast<void>(original_anchor_.abort(error));
        if (const auto actual = original_anchor_.report(); actual) observed_ = actual.value();
        control_.phase = Phase::failed; control_.pending = false; sync(); context_ = context_seal_ = nullptr;
        return error;
    }
    [[nodiscard]] bool bridge_progress_exact() const {
        if (!bridge_minted_) return !bridge_workspace_ && !bridge_child_ && !bridge_attachment_;
        if (!slot_ || bridge_workspace_ != slot_ || !bridge_child_ || !bridge_attachment_ ||
            !reclaim_history::same_session(bridge_terminal_, bridge_terminal_seal_) ||
            (control_.phase != Phase::suffix && control_.phase != Phase::closed)) return false;
        auto expected = bridge_terminal_;
        const auto reads = control_.suffix_reads;
        if (reads > ~block::SessionCounter{0} - expected.reads || reads > ~block::SessionCounter{0} - expected.completed_reads) return false;
        expected.reads += reads; expected.completed_reads += reads;
        if (!control_.pending) return reclaim_history::same_session(expected, progress_);
        if (!reclaim_history::same_session(expected, flight_)) return false;
        if (reclaim_history::same_session(expected, progress_)) return true;
        if (!control_.dispatch_seen || expected.reads == ~block::SessionCounter{0} || expected.completed_reads == ~block::SessionCounter{0}) return false;
        ++expected.reads; ++expected.completed_reads;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] bool retained(const Context &context, bool adopted = false, bool consumed = false) const {
        const bool attachment = consumed ?
            !context_ && !context_seal_ && control_.phase == Phase::closed && control_.receipt_consumed && bridge_minted_ :
            context_ == &context && context_seal_ == &context;
        return (!common_adopted_ || common_whole_ == Ext4MountedCommonOpeningBudget::whole_union()) &&
            attempted_ && attempted_seal_ && attachment &&
            original_ == &original_anchor_ && &context.session_ == &original_anchor_ && anchor_.exact(&original_anchor_) &&
            slot_ == context.slot_ && slot_seal_ == context.slot_ && bridge_progress_exact() &&
            authority_ == &context.authority_ && authority_seal_ == &context.authority_ &&
            owner_ == context.owner_ && owner_seal_ == context.owner_ && mount_ == context.mount_ && mount_seal_ == context.mount_ &&
            context.serial_ && control_.serial == context.serial_ && control_ == sealed_ &&
            reclaim_history::same_session(initial_, initial_seal_) &&
            (adopted || reclaim_history::same_session(progress_, progress_seal_)) &&
            reclaim_history::same_session(flight_, flight_seal_) && reclaim_history::same_session(paused_, paused_seal_) &&
            capacity_ == capacity_seal_ &&
            binding_ == binding_seal_ && same_geometry(geometry_, geometry_seal_) && same_bounds(bounds_, bounds_seal_) &&
            transfer_ == transfer_seal_ && origin_ == origin_seal_ && opening_ == opening_seal_ &&
            groups_ == groups_seal_ && blocks_ == blocks_seal_ && control_.classifier_reads <= capacity_.classifier_reads &&
            control_.suffix_reads <= capacity_.suffix_reads && control_.stage_reads <= control_.stage_limit;
    }
    [[nodiscard]] Status exact_volume(Context &context) const {
        const auto binding = context.session_.observe_binding();
        const auto transfer = context.session_.transfer_limit();
        if (!binding || binding.value() != binding_ || !transfer || transfer.value() != transfer_ ||
            !same_geometry(context.session_.geometry(), geometry_) || !same_bounds(context.session_.bounds(), bounds_))
            return Status::fault("mounted typed cursor original backing or volume changed");
        return {};
    }
    [[nodiscard]] Status guard(Context &context) {
        if (!retained(context)) return Status::fault("mounted typed cursor original retained context changed");
        if (auto status = exact_volume(context); !status) return status;
        if (bridge_minted_) {
            if (!context.bridge_check_) return Status::fault("mounted typed cursor original Owner bridge missing");
            if (auto status = context.bridge_check_(context.owner_, &context, bridge_attachment_); !status) return status;
        }
        const auto status = context.authority_.validate(control_.phase, control_.stage);
        if (!retained(context)) return Status::fault("mounted typed cursor retained context changed in authority callback");
        if (auto exact = exact_volume(context); !exact) return exact;
        if (bridge_minted_) {
            if (!context.bridge_check_) return Status::fault("mounted typed cursor original Owner bridge missing after observation");
            if (auto exact = context.bridge_check_(context.owner_, &context, bridge_attachment_); !exact) return exact;
        }
        return status;
    }
    [[nodiscard]] Status idle(Context &context) {
        if (auto status = guard(context); !status) return reject(status);
        const auto actual = context.session_.report();
        if (!actual || control_.pending || !reclaim_history::same_session(actual.value(), progress_))
            return reject(actual ? Status::fault("mounted typed cursor idle native counters changed") : actual.status());
        observed_ = actual.value(); return {};
    }
    template <typename Counter = block::SessionCounter>
        requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] Status reserve(Context &context, u32 groups, u64 blocks, Origin origin, Opening opening) {
        if (attempted_) return Status::already_exists("mounted typed cursor reservation is one-shot");
        if (&context.session_ != &original_anchor_) return Status::invalid_argument("mounted cursor immutable original Session differs");
        attempted_ = attempted_seal_ = true;
        if (auto status = anchor_.bind(context.session_); !status) return status;
        original_ = &context.session_;
        if (!context.serial_ || !context.owner_ || !context.mount_) return reject(Status::invalid_argument("mounted typed cursor original context"));
        const auto initial = context.session_.report(); if (!initial) return reject(initial.status());
        constexpr u64 counter_limit = block::session_counter_limit<Counter>;
        Ext4MountedTypedCapacity maximum{}; bool any{};
        for (Route route : {Route::zero_fresh, Route::zero_recovery, Route::zero_noop, Route::allocated_fresh, Route::allocated_recovery}) {
            if (!permitted(origin, opening, route)) continue;
            const auto candidate = Ext4MountedTypedBudget::capacity<Counter>(initial.value(), groups, blocks, origin, route);
            if (!candidate || !fits(initial.value(), candidate.value(), counter_limit)) {
                const auto reason = candidate ? Status::overflow("mounted typed actual native counter width exhausted") : candidate.status();
                if (reason.code() == StatusCode::fault) return reject(reason);
                control_.phase = Phase::refused; observed_ = initial.value(); sync(); return reason;
            }
            if (!any || candidate.value().reads > maximum.reads) maximum = candidate.value();
            if (candidate.value().writes > maximum.writes) maximum.writes = candidate.value().writes;
            if (candidate.value().flushes > maximum.flushes) maximum.flushes = candidate.value().flushes;
            any = true;
        }
        if (!any) { control_.phase = Phase::refused; sync(); return Status::invalid_argument("mounted typed cursor opening family"); }
        const auto binding = context.session_.observe_binding(); const auto transfer = context.session_.transfer_limit();
        if (!binding || !transfer) return reject(!binding ? binding.status() : transfer.status());
        const auto geometry = context.session_.geometry(); const auto bounds = context.session_.bounds();
        if (geometry.block_size != 512 || !geometry.block_count || !bounds.count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || blocks > geometry.block_count / 2 ||
            transfer.value() < 1024 || transfer.value() % 512)
            return reject(Status::unsupported("mounted typed cursor native volume profile"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = observed_ = initial.value();
        binding_ = binding_seal_ = binding.value(); geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        transfer_ = transfer_seal_ = transfer.value(); capacity_ = capacity_seal_ = maximum;
        context_ = context_seal_ = &context; authority_ = authority_seal_ = &context.authority_;
        slot_ = slot_seal_ = context.slot_;
        owner_ = owner_seal_ = context.owner_; mount_ = mount_seal_ = context.mount_;
        groups_ = groups_seal_ = groups; blocks_ = blocks_seal_ = blocks;
        origin_ = origin_seal_ = origin; opening_ = opening_seal_ = opening;
        control_.phase = Phase::classifier; control_.serial = context.serial_;
        control_.counter_limit = counter_limit; sync();
        return idle(context);
    }
    // Only the concrete Owner can supply the private original Selection. Its
    // authentication checks the real retained opening ledger and source bytes;
    // baseline and endpoint observations cannot independently invoke adoption.
    template <typename Selection>
    [[nodiscard]] Status adopt_common_predecessor(Context &context, const Selection &selection) {
        if (attempted_ || &context.session_ != &original_anchor_ || !context.serial_ || !context.owner_ || !context.mount_)
            return Status::invalid_argument("common cursor adoption requires its unused original context");
        if (auto status = selection.authenticate_predecessor(context); !status) return status;
        const Route route = selection.family() == Ext4MountedCommonFamily::typed_zero ? Route::zero_recovery :
            selection.family() == Ext4MountedCommonFamily::typed_allocated ? Route::allocated_recovery : Route::zero_noop;
        if (route == Route::zero_noop) return Status::unsupported("common cursor predecessor is not a typed recovery family");
        const auto available = Ext4MountedTypedBudget::capacity(selection.baseline(), 4, 32768, Origin::startup, route);
        if (!available) return available.status();
        const auto binding = context.session_.observe_binding(); const auto transfer = context.session_.transfer_limit();
        const auto geometry = context.session_.geometry(); const auto bounds = context.session_.bounds();
        if (!binding || !transfer) return !binding ? binding.status() : transfer.status();
        if (geometry.block_size != 512 || (geometry.block_count < 65536) || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || transfer.value() < 1024 || transfer.value() % 512)
            return Status::unsupported("common cursor original fixed volume profile");
        if (auto status = selection.authenticate_predecessor(context); !status) return status;
        if (auto status = anchor_.bind(context.session_); !status) return status;
        attempted_ = attempted_seal_ = true; original_ = &context.session_;
        initial_ = initial_seal_ = selection.baseline();
        progress_ = progress_seal_ = observed_ = selection.endpoint();
        binding_ = binding_seal_ = binding.value(); geometry_ = geometry_seal_ = geometry;
        bounds_ = bounds_seal_ = bounds; transfer_ = transfer_seal_ = transfer.value();
        capacity_ = capacity_seal_ = available.value();
        common_whole_ = selection.whole_union(); common_adopted_ = true;
        context_ = context_seal_ = &context; authority_ = authority_seal_ = &context.authority_;
        slot_ = slot_seal_ = context.slot_; owner_ = owner_seal_ = context.owner_; mount_ = mount_seal_ = context.mount_;
        groups_ = groups_seal_ = 4; blocks_ = blocks_seal_ = 32768;
        origin_ = origin_seal_ = Origin::startup; opening_ = opening_seal_ = Opening::startup_recovery_family;
        control_.phase = Phase::classifier; control_.serial = context.serial_;
        control_.counter_limit = block::session_counter_limit<block::SessionCounter>;
        // This credit comes from the authenticated real ledger, never from a
        // seeded native history, a reopened Session or a caller-supplied report.
        control_.classifier_reads = 5; sync();
        return {};
    }
    [[nodiscard]] Status refine(Context &context, Route route, u32 sequence) {
        if (control_.phase == Phase::refined) return Status::already_exists("mounted typed cursor refinement is one-shot");
        if (control_.phase != Phase::classifier) return Status::bad_descriptor("mounted typed cursor refinement phase");
        if (auto status = idle(context); !status) return status;
        if (!permitted(origin_, opening_, route)) return Status::unsupported("mounted typed cursor route outside reserved family");
        const auto selected = Ext4MountedTypedBudget::capacity<u64>(initial_, groups_, blocks_, origin_, route);
        const auto final = Ext4MountedTypedBudget::final_sequence(route, sequence);
        if (!selected || !final || !fits(initial_, selected.value(), control_.counter_limit)) {
            control_.phase = Phase::refused; sync(); context_ = context_seal_ = nullptr;
            return !selected ? selected.status() : !final ? final.status() : Status::overflow("mounted typed cursor refined native counters exhausted");
        }
        if (control_.classifier_reads > selected.value().classifier_reads) return reject(Status::overflow("mounted typed cursor classifier exceeded refinement"));
        capacity_ = capacity_seal_ = selected.value(); control_.final_sequence = final.value(); control_.phase = Phase::refined; sync();
        return {};
    }
    [[nodiscard]] Status pause_child(Context &context) {
        if (control_.phase != Phase::refined) return Status::bad_descriptor("mounted typed cursor child pause phase");
        if (auto status = idle(context); !status) return status;
        paused_ = paused_seal_ = progress_; control_.phase = Phase::paused_child; sync(); return {};
    }
    [[nodiscard]] Status resume_after_child(Context &context) {
        if (control_.phase != Phase::paused_child) return Status::bad_descriptor("mounted typed cursor child handoff phase");
        if (auto status = guard(context); !status) return reject(status);
        if (!reclaim_history::same_session(paused_, paused_seal_) || !reclaim_history::same_session(paused_, progress_))
            return reject(Status::fault("mounted typed cursor original paused child origin changed"));
        // No actual accepted Child friendship/proof mint is composed in this
        // leaf. Never infer successful child completion from live deltas or reports.
        return Status::unsupported("mounted typed cursor authentic completed-child bridge not composed");
    }
    [[nodiscard]] static Stage selected_stage(Origin origin, Route route, u32 index) {
        if (route == Route::zero_noop) return index == 0 ? Stage::noop_terminal : Stage::none;
        constexpr std::array stages{Stage::inspect_empty, Stage::reopen_view, Stage::stored_metadata,
            Stage::target_inode, Stage::raw_target, Stage::target_metadata, Stage::final_wrapper, Stage::startup_root};
        return index < (origin == Origin::startup ? 8U : 7U) ? stages[index] : Stage::none;
    }
    [[nodiscard]] static u64 stage_limit(const Ext4MountedTypedBounds &b, Stage stage) {
        switch (stage) {
        case Stage::inspect_empty: return b.strict_empty;
        case Stage::reopen_view: return b.view_open;
        case Stage::stored_metadata: return b.stored_metadata;
        case Stage::target_inode: case Stage::target_metadata: case Stage::startup_root: return b.decode;
        case Stage::raw_target: case Stage::final_wrapper: case Stage::noop_terminal: return 1;
        default: return 0;
        }
    }
    [[nodiscard]] Status begin_stage(Context &context, Stage stage) {
        if (control_.phase != Phase::suffix || control_.stage != Stage::none) return Status::bad_descriptor("mounted typed cursor suffix stage phase");
        if (auto status = idle(context); !status) return status;
        if (stage != selected_stage(origin_, capacity_.route, control_.stage_index)) return Status::denied("mounted typed cursor suffix order");
        const auto b = Ext4MountedTypedBudget::bounds(groups_, blocks_); if (!b) return reject(b.status());
        control_.stage = stage; control_.stage_limit = stage_limit(b.value(), stage); control_.stage_reads = 0; sync(); return {};
    }
    [[nodiscard]] Status end_stage(Context &context) {
        if (control_.phase != Phase::suffix || control_.stage == Stage::none) return Status::bad_descriptor("mounted typed cursor suffix finish phase");
        if (auto status = idle(context); !status) return status;
        if (!control_.stage_reads || (control_.stage_limit == 1 && control_.stage_reads != 1)) return Status::denied("mounted typed cursor suffix observation missing");
        if (auto status = context.authority_.complete_stage(control_.stage); !status) return reject(status);
        if (auto status = idle(context); !status) return status;
        ++control_.stage_index; control_.stage = Stage::none; control_.stage_limit = control_.stage_reads = 0; sync(); return {};
    }
    struct Dispatch final {
        Ext4MountedTypedCursor &cursor; Context &context; Session &original;
        Status begin_read(Session &session, u64 first, usize bytes) {
            if (&session != &original || first != cursor.control_.sector || bytes != cursor.control_.bytes)
                return cursor.reject_original(original, Status::fault("mounted typed cursor selected READ changed"));
            if (auto status = cursor.guard(context); !status) return cursor.reject_original(original, status);
            cursor.control_.pending = true; cursor.control_.dispatch_seen = false; cursor.sync(); return {};
        }
        Status in_flight_exact(Session &session) {
            if (&session != &original || !cursor.control_.pending)
                return cursor.reject_original(original, Status::fault("mounted typed cursor dispatch original changed"));
            if (auto status = cursor.guard(context); !status) return cursor.reject_original(original, status);
            auto expected = cursor.flight_; ++expected.reads; expected.in_flight = true;
            const auto actual = original.report();
            if (!actual || !reclaim_history::same_session(expected, actual.value()))
                return cursor.reject_original(original, actual ? Status::fault("mounted typed cursor exact in-flight counters changed") : actual.status());
            cursor.control_.dispatch_seen = true; cursor.sync(); return {};
        }
        Status finish_read(Session &session, Status transport) {
            if (&session != &original) return cursor.reject_original(original, Status::fault("mounted typed cursor finish original changed"));
            if (!transport) return cursor.reject_original(original, transport);
            if (auto status = cursor.guard(context); !status) return cursor.reject_original(original, status);
            if (!cursor.control_.pending || !cursor.control_.dispatch_seen)
                return cursor.reject_original(original, Status::fault("mounted typed cursor finish lacks actual dispatch"));
            auto expected = cursor.flight_; ++expected.reads; ++expected.completed_reads;
            const auto actual = original.report();
            if (!actual || !reclaim_history::same_session(expected, actual.value()))
                return cursor.reject_original(original, actual ? Status::fault("mounted typed cursor exact READ completion changed") : actual.status());
            cursor.observed_ = actual.value(); return {};
        }
    };
    template <typename External> struct DispatchWithHook final {
        Dispatch &native; External &external;
        Status begin_read(Session &s, u64 first, usize bytes) {
            const auto status = external.begin_read(s, first, bytes);
            if (auto exact = native.cursor.guard(native.context); !exact) return native.cursor.reject_original(native.original, exact);
            if (!status) return status;
            return native.begin_read(s, first, bytes);
        }
        Status in_flight_exact(Session &s) {
            if (auto status = native.in_flight_exact(s); !status) return status;
            if constexpr (requires { { external.in_flight_exact(s) } -> std::same_as<Status>; }) {
                const auto status = external.in_flight_exact(s);
                if (auto exact = native.in_flight_exact(s); !exact) return exact;
                return status;
            }
            return {};
        }
        Status finish_read(Session &s, Status transport) {
            const auto checked = native.finish_read(s, transport);
            const auto status = external.finish_read(s, transport);
            if (!transport) return transport;
            if (!checked) return checked;
            if (auto exact = native.cursor.guard(native.context); !exact) return native.cursor.reject_original(native.original, exact);
            return status;
        }
    };
    template <typename External> requires block::ReadDispatchGuard<External, Session>
    [[nodiscard]] Status read(Context &context, u64 first, std::span<std::byte> bytes, External &external) {
        if (control_.phase == Phase::paused_child) return Status::busy("mounted typed cursor excludes paused child transport");
        if (control_.phase != Phase::classifier && control_.phase != Phase::refined && control_.phase != Phase::suffix)
            return Status::bad_descriptor("mounted typed cursor READ phase");
        if (auto status = idle(context); !status) return status;
        const u64 used = control_.phase == Phase::suffix ? control_.suffix_reads : control_.classifier_reads;
        const u64 limit = control_.phase == Phase::suffix ? capacity_.suffix_reads : capacity_.classifier_reads;
        if (used == limit || (control_.phase == Phase::suffix &&
            (control_.stage == Stage::none || control_.stage_reads == control_.stage_limit)))
            return Status::overflow("mounted typed cursor finite READ stage exhausted");
        if (bytes.size() != 1024 || first % 2 || first >= geometry_.block_count || 2 > geometry_.block_count - first)
            return Status::invalid_argument("mounted typed cursor selected 1 KiB READ bounds");
        Session &original = context.session_;
        const auto dispatch_origin = progress_;
        flight_ = flight_seal_ = progress_; control_.sector = first; control_.bytes = bytes.size(); sync();
        Dispatch native{*this, context, original}; DispatchWithHook<External> hook{native, external};
        auto authority = [&]() -> Status { return guard(context); };
        const auto status = guarded_ext4_direct_read(original, progress_, authority, first, bytes, hook);
        if (!status) return reject_original(original, status);
        auto expected = dispatch_origin; ++expected.reads; ++expected.completed_reads;
        const auto actual = original.report();
        if (!retained(context, true) || !reclaim_history::same_session(progress_seal_, dispatch_origin) ||
            !reclaim_history::same_session(flight_, dispatch_origin) || !actual ||
            !reclaim_history::same_session(expected, actual.value()) ||
            !reclaim_history::same_session(expected, progress_))
            return reject_original(original, actual ? Status::fault("mounted typed cursor complete READ adoption changed") : actual.status());
        progress_seal_ = progress_; observed_ = actual.value();
        if (auto exact = guard(context); !exact) return reject_original(original, exact);
        const auto final = original.report();
        if (!final || !reclaim_history::same_session(expected, final.value()) ||
            !reclaim_history::same_session(expected, progress_))
            return reject_original(original, final ? Status::fault("mounted typed cursor post-adoption native endpoint changed") : final.status());
        if (control_.phase == Phase::suffix) { ++control_.suffix_reads; ++control_.stage_reads; }
        else ++control_.classifier_reads;
        control_.pending = control_.dispatch_seen = false; control_.sector = 0; control_.bytes = 0; sync(); return {};
    }
    [[nodiscard]] Status read(Context &context, u64 first, std::span<std::byte> bytes) {
        Ext4DirectReadHooks hook; return read(context, first, bytes, hook);
    }
    [[nodiscard]] Status cancel_before_child(Context &context, Status reason) {
        if (control_.phase != Phase::classifier && control_.phase != Phase::refined)
            return Status::bad_descriptor("mounted typed cursor clean refusal phase");
        if (auto status = idle(context); !status) return status;
        control_.phase = Phase::refused; sync(); context_ = context_seal_ = nullptr; return reason;
    }
    [[nodiscard]] Status close_suffix(Context &context, Receipt &receipt) {
        if (receipt.minted_) return Status::already_exists("mounted typed cursor receipt is one-shot");
        if (control_.phase != Phase::suffix || control_.stage != Stage::none ||
            selected_stage(origin_, capacity_.route, control_.stage_index) != Stage::none)
            return Status::bad_descriptor("mounted typed cursor suffix is incomplete");
        if (auto status = idle(context); !status) return status;
        receipt.cursor_ = this; receipt.context_ = &context; receipt.session_ = &context.session_;
        receipt.owner_ = context.owner_; receipt.mount_ = context.mount_; receipt.serial_ = context.serial_;
        receipt.sequence_ = control_.final_sequence; receipt.endpoint_ = progress_; receipt.minted_ = true;
        control_.phase = Phase::closed; sync(); return {};
    }
    [[nodiscard]] Status consume_receipt(Context &context, Receipt &receipt) {
        if (receipt.consumed_ || control_.receipt_consumed) return Status::already_exists("mounted typed cursor receipt consumed");
        if (control_.phase != Phase::closed || !receipt.minted_ || receipt.cursor_ != this || receipt.context_ != &context ||
            receipt.session_ != &context.session_ || receipt.owner_ != context.owner_ || receipt.mount_ != context.mount_ ||
            receipt.serial_ != context.serial_ || receipt.sequence_ != control_.final_sequence ||
            !reclaim_history::same_session(receipt.endpoint_, progress_))
            return reject(Status::fault("mounted typed cursor original suffix receipt changed"));
        if (auto status = idle(context); !status) return status;
        receipt.consumed_ = true; control_.receipt_consumed = true; sync(); context_ = context_seal_ = nullptr; return {};
    }
    // The immutable native reference survives every retained-pointer/anchor
    // observation and remains alive until the cursor's terminal destruction.
    Session &original_anchor_;
    const void *slot_{}, *slot_seal_{};
    const void *bridge_workspace_{}, *bridge_child_{}, *bridge_attachment_{};
    block::SessionReport bridge_terminal_{}, bridge_terminal_seal_{};
    bool bridge_minted_{};
    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}; Context *context_{}, *context_seal_{}; Authority *authority_{}, *authority_seal_{};
    const void *owner_{}, *owner_seal_{}, *mount_{}, *mount_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, paused_{}, paused_seal_{}, observed_{};
    block::SessionBindingObservation binding_{}, binding_seal_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{}; block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4MountedTypedCapacity capacity_{}, capacity_seal_{};
    Ext4MountedCommonCapacity common_whole_{};
    bool common_adopted_{};
    Control control_{}, sealed_{}; usize transfer_{}, transfer_seal_{};
    Origin origin_{}, origin_seal_{}; Opening opening_{}, opening_seal_{};
    u32 groups_{}, groups_seal_{}; u64 blocks_{}, blocks_seal_{}; bool attempted_{}, attempted_seal_{};
};

template <usize> class Ext4MountedMetadata;
enum class Ext4StartupFamily : u8 { ordinary, namespace_active, reclaim_active, empty_orphan, rejected, legacy_active, typed_zero_active, typed_allocated_active, regular_move_active };
enum class Ext4StartupPhase : u8 { unused, classifying, namespace_checkpointing, reclaim_checkpointing, draining, admitting, complete, blocked, failed, regular_move_checkpointing };
struct Ext4StartupRecoveryReport
{
    Ext4StartupPhase phase{Ext4StartupPhase::unused};
    Ext4StartupFamily initial_family{Ext4StartupFamily::rejected};
    Status status{};
    // Legacy terminal observation precedes ordinary Node construction. Typed
    // common completion records the genuine published root after renewal.
    block::SessionReport original{}, observed{};
    u32 initial_head{}, final_head{}, initial_sequence{}, final_sequence{};
    usize chain_members{}, reclaimed{}, released_blocks{}, verified_publications{};
    usize prospective_members{}, namespace_checkpoints{}, regular_move_checkpoints{};
    bool namespace_result_validated{}, reclaim_result_validated{}, reclaim_dry_route_validated{};
    bool regular_move_result_validated{};
    bool strict_ordinary_admission{}, root_unpublished{true};
};

// Permanent private off-stack storage, disjoint from the live reclaim workspace.
class Ext4MountedStartupWorkspace final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
  public:
    Ext4MountedStartupWorkspace() = default;
    Ext4MountedStartupWorkspace(const Ext4MountedStartupWorkspace &) = delete;
    Ext4MountedStartupWorkspace &operator=(const Ext4MountedStartupWorkspace &) = delete;
    Ext4MountedStartupWorkspace(Ext4MountedStartupWorkspace &&) = delete;
    Ext4MountedStartupWorkspace &operator=(Ext4MountedStartupWorkspace &&) = delete;
  private:
    Status claim(const void *mount, const void *owner)
    {
        smp::ScopedSpinLock guard(lock_);
        if (!mount || !owner || claimant_) return Status::busy("startup workspace already permanently claimed");
        claimant_ = mount; owner_ = owner; return {};
    }
    const void *claimant_{}, *owner_{};
    smp::SpinLock lock_{};
    Ext4OrphanChainCaptureStorage original_{}, sealed_original_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch preparation_{};
    Ext4OrphanReclaimCaptureStorage observed_{};
    Ext4OrphanReclaimColdScratch verification_{};
    Ext4RecoveryBinding terminal_binding_{};
    Ext4ReclaimRecoveryStorage active_reclaim_{};
    Ext4NamespaceRecoveryBinding active_namespace_{};
    Ext4NamespaceResultHistory namespace_history_{};
    Ext4OrphanChainCaptureStorage namespace_result_{};
    Ext4NamespaceSnapshotSeal namespace_result_seal_{};
    Ext4OrphanChainCaptureStorage reclaim_result_{};
    Ext4NamespaceSnapshotSeal reclaim_result_seal_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch reclaim_preparation_{};
    Ext4RecoveryBinding reclaim_ordinary_{};
    Ext4ReclaimComposedStorage reclaim_composed_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch namespace_preparation_{};
    Ext4OrphanBlock locator_super_{}, locator_descriptors_{}, locator_table_{}, locator_head_{}, locator_descriptor_{};
    Ext4OrphanBlock legacy_head_seal_{}, legacy_descriptor_seal_{};
};

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

    #include "ok/fs/ext4/detail/owned_regular_move.inc"

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
    #include "ok/fs/ext4/detail/owned_typed_port.inc"
    #include "ok/fs/ext4/detail/owned_mounted_cursor.inc"
    #include "ok/fs/ext4/detail/owned_common_opening.inc"
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
    #include "ok/fs/ext4/detail/mounted_startup_port.inc"

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

enum class Ext4MountedPolicy : u8
{ maintenance_only, maintained_noatime, ordered_data_noatime, ordered_data_dirty_noatime };

enum class Ext4MountPhase : u8 { unused, opening, readable, updating, draining, freezing, clean_sealed, failed, closed };
struct Ext4MountReport
{
    Ext4MountPhase phase{Ext4MountPhase::unused};
    Status status{};
    u64 epoch{};
    bool published{}, reading{};
};

// Kernel-owned exclusive off-stack storage. It is bound once to one mount
// lifetime; ordinary user programs never receive these internal addresses.
struct Ext4MountedReclaimWorkspace final
{
    template <usize> friend class Ext4MountedMetadata;
    Ext4MountedReclaimWorkspace() = default;
    Ext4MountedReclaimWorkspace(const Ext4MountedReclaimWorkspace &) = delete;
    Ext4MountedReclaimWorkspace &operator=(const Ext4MountedReclaimWorkspace &) = delete;
    Ext4MountedReclaimWorkspace(Ext4MountedReclaimWorkspace &&) = delete;
    Ext4MountedReclaimWorkspace &operator=(Ext4MountedReclaimWorkspace &&) = delete;
  private:
    Status claim(const void *mount)
    {
        smp::ScopedSpinLock guard(lock);
        if (!mount || claimant) return Status::busy("reclaim workspace already belongs to a mount lifetime");
        claimant = mount; return {};
    }
    Ext4OrphanChainCaptureStorage original{};
    Ext4OrphanDirectoryQualifiedReclaimScratch preparation{};
    Ext4OrphanReclaimCaptureStorage observed{};
    Ext4OrphanReclaimColdScratch verification{};
    const void *claimant{};
    smp::SpinLock lock{};
};

// A bounded mounted metadata and ordered-data writer. Transport,
// registry and provider outlive every token, Node retain and synchronous call.
// Detachment/drain are one-way; external storage destruction is serialized.
template <usize DeviceCapacity = 4>
class Ext4MountedMetadata final
{
    // Single-block mkdir needs eight home images plus the journal head. The
    // earlier regular-file transaction still admits its seven-buffer policy.
    using Owner = Ext4OwnedTransaction<DeviceCapacity, ext4_directory_create_owner_buffers>;
    using Lifetime = Ext4InodeLifetimeTable<128, 512>;
    using Lease = Lifetime::Lease;
    struct Reader
    {
        Ext4MountedMetadata *mount;
        Status read_block(u32 block, std::span<u8> output)
        {
            typename Owner::Pin pin;
            const auto pinned = mount->owner_.common_root_domain_active() ?
                mount->owner_.common_root_pin(block, pin) : mount->owner_.startup_domain_active() ?
                mount->owner_.startup_pin(block, pin) : mount->owner_.pin(block, pin);
            if (!pinned) return pinned;
            auto status = pin.read(0, std::as_writable_bytes(output));
            const auto released = pin.release();
            return status ? released : status;
        }
    };
    using View = Ext4ReadOnlyView<Reader>;
    // Off-stack observations copied only from the original Owner's actual
    // selected completed child. This private record grants no dispatch right.
    struct TypedOwnershipSnapshot final
    {
        struct Group { u32 block_bitmap{}, inode_bitmap{}, table{}; u16 block_checksum{}, inode_checksum{}, flags{};
            friend bool operator==(const Group &, const Group &) = default; };
        struct Range { u32 first{}, count{}; friend bool operator==(const Range &, const Range &) = default; };
        TypedOwnershipSnapshot() = default;
        TypedOwnershipSnapshot(const TypedOwnershipSnapshot &) = delete;
        TypedOwnershipSnapshot &operator=(const TypedOwnershipSnapshot &) = delete;
        TypedOwnershipSnapshot(TypedOwnershipSnapshot &&) = delete;
        TypedOwnershipSnapshot &operator=(TypedOwnershipSnapshot &&) = delete;
        const Ext4MountedMetadata *mount{}; const Owner *owner{};
        const typename Owner::TypedOwnerWorkspace *workspace{};
        const void *child{}, *view{}, *reader{}, *proof{};
        Ext4RecoveryInfo binding{}; block::SessionReport terminal{};
        std::array<u8, 16> uuid{};
        std::array<Group, Ext4RecoveryInfo::group_capacity> groups{};
        std::array<Range, 128 * 4> ranges{};
        usize range_count{};
        u32 seed{}, first_data{}, blocks_per_group{}, inodes_per_group{}, group_count{}, inodes{}, first_inode{}, table_blocks{};
        u32 retained_orphan{}, retained_orphan_generation{}, retained_orphan_next{};
        Ext4ZeroTruncateOwnership target{};
        bool attempted{}, ready{}, failed{}, allocated{}, noop{}, minted{};
    };
    template <typename ChildReader>
    [[nodiscard]] static bool typed_view_snapshot_exact(const Ext4ReadOnlyView<ChildReader> &view,
        const Ext4ZeroTruncateOwnership &proof, const TypedOwnershipSnapshot &snapshot)
    {
        const auto &layout = view.layout_; const auto &saved = snapshot.binding.layout;
        if (snapshot.view != &view || snapshot.reader != view.reader_ || snapshot.proof != &proof ||
            view.attempted_ != snapshot.attempted || view.ready_ != snapshot.ready || view.failed_ != snapshot.failed ||
            layout.first_sector != saved.first_sector || layout.filesystem_blocks != saved.filesystem_blocks ||
            layout.journal_first_block != saved.journal_first_block || layout.journal_blocks != saved.journal_blocks ||
            layout.block_size != saved.block_size || view.uuid_ != snapshot.uuid ||
            view.seed_ != snapshot.seed || view.first_data_ != snapshot.first_data ||
            view.blocks_per_group_ != snapshot.blocks_per_group || view.inodes_per_group_ != snapshot.inodes_per_group ||
            view.group_count_ != snapshot.group_count || view.inodes_ != snapshot.inodes ||
            view.first_inode_ != snapshot.first_inode || view.table_blocks_ != snapshot.table_blocks ||
            view.ownership_range_count_ != snapshot.range_count ||
            view.retained_orphan_ != snapshot.retained_orphan ||
            view.retained_orphan_generation_ != snapshot.retained_orphan_generation ||
            view.retained_orphan_next_ != snapshot.retained_orphan_next ||
            proof.inode != snapshot.target.inode || proof.generation != snapshot.target.generation ||
            proof.old_size != snapshot.target.old_size || proof.count != snapshot.target.count ||
            proof.group != snapshot.target.group || proof.mapping != snapshot.target.mapping) return false;
        for (usize at = 0; at < snapshot.groups.size(); ++at)
        {
            const auto &actual = view.groups_[at];
            if (snapshot.groups[at] != typename TypedOwnershipSnapshot::Group{actual.block_bitmap,
                actual.inode_bitmap, actual.table, actual.block_checksum, actual.inode_checksum, actual.flags}) return false;
        }
        for (usize at = 0; at < snapshot.ranges.size(); ++at)
            if (snapshot.ranges[at] != typename TypedOwnershipSnapshot::Range{
                view.ownership_ranges_[at].first, view.ownership_ranges_[at].count}) return false;
        return true;
    }
    [[nodiscard]] Status capture_typed_ownership_snapshot(typename Owner::TypedOwnerWorkspace &workspace,
                                                        TypedOwnershipSnapshot &snapshot)
    {
        const auto overlaps = [](const void *a, usize as, const void *b, usize bs) {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < as : one - two < bs;
        };
        if (overlaps(&snapshot, sizeof(snapshot), this, sizeof(*this)) ||
            overlaps(&snapshot, sizeof(snapshot), &workspace, sizeof(workspace)))
            return Status::invalid_argument("typed completed ownership snapshot aliases retained storage");
        if (snapshot.minted) return Status::already_exists("typed completed ownership snapshot is one-shot");
        auto capture = [&](const auto &view, const Ext4ZeroTruncateOwnership &proof, const Ext4RecoveryInfo &binding,
            const block::SessionReport &terminal, const void *child, bool allocated, bool noop) -> Status {
            if (!view.reader_ || !view.attempted_ || !view.ready_ || view.failed_ ||
                view.ownership_range_count_ > view.ownership_ranges_.size())
                return Status::fault("typed completed ownership actual view is unavailable");
            snapshot.mount = this; snapshot.owner = &owner_; snapshot.workspace = &workspace;
            snapshot.child = child; snapshot.view = &view; snapshot.reader = view.reader_; snapshot.proof = &proof;
            snapshot.binding = binding; snapshot.terminal = terminal; snapshot.uuid = view.uuid_;
            snapshot.seed = view.seed_; snapshot.first_data = view.first_data_;
            snapshot.blocks_per_group = view.blocks_per_group_; snapshot.inodes_per_group = view.inodes_per_group_;
            snapshot.group_count = view.group_count_; snapshot.inodes = view.inodes_;
            snapshot.first_inode = view.first_inode_; snapshot.table_blocks = view.table_blocks_;
            snapshot.retained_orphan = view.retained_orphan_;
            snapshot.retained_orphan_generation = view.retained_orphan_generation_;
            snapshot.retained_orphan_next = view.retained_orphan_next_;
            snapshot.attempted = view.attempted_; snapshot.ready = view.ready_; snapshot.failed = view.failed_;
            snapshot.range_count = view.ownership_range_count_; snapshot.target = proof;
            snapshot.allocated = allocated; snapshot.noop = noop;
            for (usize at = 0; at < snapshot.groups.size(); ++at)
            {
                const auto &group = view.groups_[at];
                snapshot.groups[at] = {group.block_bitmap, group.inode_bitmap, group.table,
                    group.block_checksum, group.inode_checksum, group.flags};
            }
            for (usize at = 0; at < snapshot.ranges.size(); ++at)
                snapshot.ranges[at] = {view.ownership_ranges_[at].first, view.ownership_ranges_[at].count};
            return typed_view_snapshot_exact(view, proof, snapshot) ? Status{} :
                Status::fault("typed completed ownership initial declared fields differ");
        };
        const auto status = owner_.typed_visit_completed_view(workspace, capture);
        if (status) snapshot.minted = true;
        return status;
    }
    [[nodiscard]] Status validate_typed_ownership_snapshot(typename Owner::TypedOwnerWorkspace &workspace,
                                                         const TypedOwnershipSnapshot &snapshot)
    {
        if (!snapshot.minted || snapshot.mount != this || snapshot.owner != &owner_ || snapshot.workspace != &workspace)
            return Status::fault("typed completed ownership snapshot original attachment differs");
        auto validate = [&](const auto &view, const Ext4ZeroTruncateOwnership &proof, const Ext4RecoveryInfo &binding,
            const block::SessionReport &terminal, const void *child, bool allocated, bool noop) -> Status {
            if (snapshot.child != child || snapshot.allocated != allocated || snapshot.noop != noop ||
                !reclaim_history::same_info(snapshot.binding, binding) ||
                !reclaim_history::same_session(snapshot.terminal, terminal) ||
                !typed_view_snapshot_exact(view, proof, snapshot))
                return Status::fault("typed completed ownership selected actual fields changed");
            return {};
        };
        return owner_.typed_visit_completed_view(workspace, validate);
    }
    class MountedNode final : public Node
    {
        friend class Ext4MountedMetadata;
      public:
        std::string_view name() const override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            return rename_count_ ? rename_names_[rename_count_ - 1].view() : name_.view();
        }
        // Legacy diagnostic accessor cannot convey an error. Qualified callers
        // must use query_metadata; fallback has no permissions or valid identity.
        Metadata metadata() const override
        {
            auto result = query_metadata();
            return result ? result.value() : Metadata{.mode = 0, .link_count = 0, .block_size = 0};
        }
        Result<Metadata> query_metadata() const override
        { return mount_->read_operation([&] { return mount_->metadata(inode_); }, false, &lifetime_key_); }
        const void *filesystem_identity() const override { return mount_; }
        Result<Node *> lookup_child(std::string_view name) override
        {
            return mount_->read_operation([&]() -> Result<Node *> {
                auto child = mount_->view_.lookup(inode_, name);
                if (!child) return child.status();
                auto found = mount_->node(child.value(), name);
                if (!found) return found.status();
                if (auto status = found.value()->expose_borrowed_identity(); !status) return status;
                return found.value();
            }, false, &lifetime_key_);
        }
        Result<Node *> lookup_child_retained(std::string_view name) override
        {
            Node *retained{};
            auto found = mount_->read_operation([&]() -> Result<Node *> {
                auto child = mount_->view_.lookup(inode_, name);
                if (!child) return child.status();
                auto node = mount_->node(child.value(), name);
                if (!node) return node.status();
                if (auto status = node.value()->retain(); !status) return status;
                retained = node.value();
                return retained;
            }, false, &lifetime_key_);
            if (!found && retained)
            {
                if (auto status = retained->release(); !status) return status;
            }
            return found;
        }
        Node *lookup(std::string_view name) override
        { auto result = lookup_child(name); return result ? result.value() : nullptr; }
        Result<FileBuffer> read(usize offset, usize count) const override
        {
            return mount_->read_operation([&]() -> Result<FileBuffer> {
                FileBuffer bytes{};
                auto result = mount_->view_.read(inode_, offset,
                    {reinterpret_cast<u8 *>(bytes.data.data()), count < bytes.data.size() ? count : bytes.data.size()});
                if (!result) return result.status();
                bytes.size = result.value(); return bytes;
            }, false, &lifetime_key_);
        }
        Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override
        {
            Result<DirectoryListing> result{std::in_place};
            auto &listing = result.value();
            const auto status = mount_->read_operation([&]() -> Status {
                return mount_->view_.directory(inode_, [&](const Ext4ReadOnlyDirent &entry) -> Status {
                    if (entry.name == "." || entry.name == "..") return {};
                    if (listing.count == listing.entries.size()) return Status::overflow("mounted directory capacity");
                    auto metadata = mount_->metadata(entry.inode);
                    if (!metadata) return metadata.status();
                    auto &item = listing.entries[listing.count];
                    if (auto copied = item.name.assign(entry.name); !copied) return copied;
                    item.metadata = metadata.value(); ++listing.count; return {};
                });
            }, false, &lifetime_key_);
            if (!status) return status;
            return result;
        }
        Result<FixedString<96>> directory_path(const Node &root) const override
        {
            if (root.filesystem_identity() != mount_) return Status::not_found("foreign mounted directory root");
            // Verify identity by address, without trusting an unrelated Node cast.
            u32 root_inode{};
            {
                smp::ScopedSpinLock guard(mount_->lock_);
                for (const auto &node : mount_->nodes_) if (&node == &root) root_inode = node.inode_;
            }
            if (!root_inode) return Status::not_found("unknown mounted root");
            return mount_->read_operation([&]() -> Result<FixedString<96>> {
                u32 current = inode_; FixedString<96> suffix;
                for (usize depth = 0; depth < 48; ++depth)
                {
                    auto metadata = mount_->metadata(current);
                    if (!metadata) return metadata.status();
                    if (metadata.value().type != NodeType::directory) return Status::not_directory("mounted path node");
                    if (current == root_inode) return suffix;
                    if (current == 2) return Status::not_found("mounted directory outside root");
                    auto parent = mount_->view_.lookup(current, "..");
                    if (!parent) return parent.status();
                    if (parent.value() == current) return Status::io_error("mounted parent cycle");
                    FixedString<max_path_segment> name; bool found{};
                    auto status = mount_->view_.directory(parent.value(), [&](const Ext4ReadOnlyDirent &entry) -> Status {
                        if (entry.inode != current || entry.name == "." || entry.name == "..") return {};
                        if (found) return Status::io_error("ambiguous mounted directory parent");
                        found = true; return name.assign(entry.name);
                    });
                    if (!status) return status;
                    if (!found) return Status::io_error("mounted parent lacks child");
                    FixedString<96> path;
                    if (!path.append('/') || !path.append(name.view()) || !path.append(suffix.view()))
                        return Status::overflow("mounted directory path capacity");
                    suffix = path; current = parent.value();
                }
                return Status::overflow("mounted directory ancestry capacity");
            }, false, &lifetime_key_);
        }
        Status retain() override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            if (auto status = mount_->admission(); !status) return status;
            return retain_locked();
        }
        Status expose_borrowed_identity() override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            if (auto status = mount_->admission(); !status) return status;
            if (!inode_) return Status::not_found("mounted borrowed identity is unavailable");
            borrowed_identity_exposed_ = true; mount_->legacy_exposure_ = true;
            return {};
        }
        Status release() override
        {
            bool consumed{};
            Status integrity{};
            {
                smp::ScopedSpinLock guard(mount_->lock_);
                integrity = mount_->retention_integrity_locked();
                if (!references_)
                {
                    for (const auto &held : mount_->reference_holds_)
                        if (held.node == this)
                            return mount_->fail_locked(Status::fault("mounted zero count has a Node lease"));
                    const auto state = mount_->lifetime_.snapshot(lifetime_key_);
                    if (state && state.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)])
                        return mount_->fail_locked(Status::fault("mounted zero count has an inode lease"));
                    return Status::bad_descriptor("mounted node has no reference");
                }
                for (auto &held : mount_->reference_holds_)
                    if (held.node == this)
                    {
                        if (!held.lease || !held.lease->valid())
                            return mount_->fail_locked(Status::fault("mounted release lease is absent"));
                        if (auto status = mount_->lifetime_.validate_lease(*held.lease, lifetime_key_,
                                                                         Ext4InodeHold::descriptor); !status)
                            return mount_->fail_locked(status);
                        const auto status = mount_->lifetime_.release(*held.lease);
                        if (!status) return mount_->fail_locked(status);
                        held.lease.reset(); held.node = nullptr; --references_; consumed = true; break;
                    }
            }
            if (!consumed) return mount_->fail(Status::fault("mounted inode reference count differs from leases"));
            if (!integrity) return integrity;
            // I/O may happen only after dropping the mount/lifetime locks.
            return mount_->drain_pending_orphan();
        }
        Status sync() override { return mount_->sync(); }
        Status sync_persistent() override { return mount_->sync_persistent(*this); }
        Status write(usize offset, std::span<const std::byte> data) override
        { return mount_->write_regular(*this, offset, data); }
        Status truncate(usize size) override { return mount_->truncate_regular(*this, size); }
        Status create(std::string_view, NodeType) override { return mount_->unsupported(); }
        Result<Node *> create_child(std::string_view name, const InitialNodeAttributes &attributes) override
        { return mount_->create_node(*this, name, attributes, false); }
        Result<Node *> create_child_retained(std::string_view name, const InitialNodeAttributes &attributes) override
        { return mount_->create_node(*this, name, attributes, true); }
        Status chmod(u32) override { return mount_->unsupported(); }
        ModeChangeResult change_mode(u32 mode, user::Credentials caller) override
        { return mount_->change_metadata<false>(*this, mode, 0, caller); }
        ModeChangeResult change_owner(u32 uid, u32 gid, user::Credentials caller) override
        { return mount_->change_owner(*this, uid, gid, caller); }
        Status chown(u32, u32) override { return mount_->unsupported(); }
        Status remove(std::string_view name, bool directory) override
        { return mount_->namespace_regular(*this, name, {}, directory, false); }
        Status link(std::string_view, Node &) override { return mount_->unsupported(); }
        Status rename(std::string_view old_name, Node &destination, std::string_view new_name) override
        { return mount_->rename_dispatch(*this, old_name, destination, new_name); }
        Status create_symlink(std::string_view, std::string_view, u32, u32) override { return mount_->unsupported(); }
      private:
        // Mount lock held; lifetime acquisition performs no I/O.
        Status retain_locked()
        {
            if (references_ == ~usize{0}) return Status::overflow("mounted node references");
            for (auto &held : mount_->reference_holds_)
                if (!held.node)
                {
                    auto lease = mount_->retain_existing(lifetime_key_, Ext4InodeHold::descriptor);
                    if (!lease) return lease.status();
                    held.lease.emplace(std::move(lease).value());
                    held.node = this;
                    ++references_; return {};
                }
            return Status::no_memory("mounted inode reference capacity");
        }
        Ext4MountedMetadata *mount_{};
        u32 inode_{};
        usize references_{};
        bool borrowed_identity_exposed_{};
        Ext4InodeLifetimeKey lifetime_key_{};
        FixedString<max_path_segment> name_{};
        // Previously returned views remain immutable until identity retirement.
        // The caller must retain the identity for its borrowed string lifetime.
        std::array<FixedString<max_path_segment>, 15> rename_names_{};
        usize rename_count_{};
    };

    struct ReferenceHold
    {
        MountedNode *node{};
        std::optional<Lease> lease{};
    };
    // Synchronous stack-owned original authority. Mutable mount fields are
    // checked against this object, and cannot erase the operation's existence.
    struct NodeMutationContext final
    {
        NodeMutationContext(MountedNode &value, Ext4InodeLifetimeKey identity, Lease &held, u64 admitted)
            : node(value), key(identity), lease(held), epoch(admitted) {}
        NodeMutationContext(const NodeMutationContext &) = delete;
        NodeMutationContext &operator=(const NodeMutationContext &) = delete;
        NodeMutationContext(NodeMutationContext &&) = delete;
        NodeMutationContext &operator=(NodeMutationContext &&) = delete;
        MountedNode &node;
        const Ext4InodeLifetimeKey key;
        Lease &lease;
        const u64 epoch;
    };

#include "ok/fs/ext4/detail/mounted_original_node_authority.inc"
#include "ok/fs/ext4/detail/mounted_typed_suffix.inc"
#include "ok/fs/ext4/detail/mounted_typed_publication.inc"
#include "ok/fs/ext4/detail/mounted_typed_request.inc"
#include "ok/fs/ext4/detail/mounted_typed_cold_startup.inc"
#include "ok/fs/ext4/detail/mounted_common_opening.inc"

  public:
    using Registry = typename Owner::Registry;
    // Caller-owned permanent storage; allocate outside synchronous call stacks.
    // Registry/provider and this storage outlive Mount and all retained Nodes.
    class CommonStorage final
    {
        friend class Ext4MountedMetadata;
      public:
        CommonStorage() = default;
        ~CommonStorage() = default;
        CommonStorage(const CommonStorage &) = delete;
        CommonStorage &operator=(const CommonStorage &) = delete;
        CommonStorage(CommonStorage &&) = delete;
        CommonStorage &operator=(CommonStorage &&) = delete;
      private:
        Ext4MountedStartupWorkspace startup_{};
        Ext4MountedReclaimWorkspace reclaim_{};
        typename Owner::TypedOwnerWorkspace child_{};
        std::optional<TypedColdWorkspace> operation_{};
        std::optional<TypedColdPublication> publication_{};
        std::optional<TypedMutationStorage> requests_{};
        const Ext4MountedMetadata *claimant_{};
        smp::SpinLock lock_{};
    };
    explicit Ext4MountedMetadata(CommonStorage &storage)
        : Ext4MountedMetadata(Ext4MountedPolicy::ordered_data_dirty_noatime,
                              &storage.reclaim_, &storage.startup_)
    { common_storage_ = &storage; }
    explicit Ext4MountedMetadata(Ext4MountedPolicy policy = Ext4MountedPolicy::maintenance_only,
                                 Ext4MountedReclaimWorkspace *workspace = nullptr,
                                 Ext4MountedStartupWorkspace *startup = nullptr)
        : policy_(policy), reclaim_workspace_(workspace), startup_workspace_(startup), owner_(policy == Ext4MountedPolicy::ordered_data_dirty_noatime),
          inspection_(policy == Ext4MountedPolicy::ordered_data_dirty_noatime,
                      policy == Ext4MountedPolicy::ordered_data_dirty_noatime) {}
    [[nodiscard]] Ext4MountedPolicy policy() const { return policy_; }
    Ext4MountedMetadata(const Ext4MountedMetadata &) = delete;
    Ext4MountedMetadata &operator=(const Ext4MountedMetadata &) = delete;
    Ext4MountedMetadata(Ext4MountedMetadata &&) = delete;
    Ext4MountedMetadata &operator=(Ext4MountedMetadata &&) = delete;
    [[nodiscard]] Status mount(Registry &registry, typename Registry::Registration key, block::SessionBounds bounds)
    {
        if (common_storage_) return mount_common_storage(registry, key, bounds);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4MountPhase::unused) return Status::already_exists("metadata mount is one-shot");
            if (policy_ != Ext4MountedPolicy::maintenance_only && policy_ != Ext4MountedPolicy::maintained_noatime &&
                policy_ != Ext4MountedPolicy::ordered_data_noatime &&
                policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
                return Status::invalid_argument("unknown mounted metadata policy");
            report_.phase = Ext4MountPhase::opening;
        }
        if (startup_workspace_)
        {
            const auto overlaps = [](const void *a, usize na, const void *b, usize nb) {
                const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
                return x <= y ? y - x < na : x - y < nb;
            };
            Status status;
            if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime ||
                overlaps(startup_workspace_, sizeof(*startup_workspace_), this, sizeof(*this)) ||
                (reclaim_workspace_ && overlaps(startup_workspace_, sizeof(*startup_workspace_),
                                               reclaim_workspace_, sizeof(*reclaim_workspace_))))
                status = Status::invalid_argument("startup workspace policy or overlap");
            else status = startup_workspace_->claim(this, &owner_);
            if (!status)
            { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status; }
        }
        if (reclaim_workspace_)
            if (auto status = reclaim_workspace_->claim(this); !status)
            { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status; }
        if (auto status = owner_.open(registry, key, bounds); !status) return fail(status);
        identity_ = owner_.buffers();
        if (startup_workspace_)
            if (auto status = owner_.recover_startup(*startup_workspace_, this, 1, startup_reader_, startup_ordinary_reader_); !status) return fail(status);
        return mount_after_startup();
    }
    [[nodiscard]] Result<MountedRootReference> retain_root()
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = admission(); !status) return status;
        if (auto status = identity(); !status) return fail_locked(status);
        auto reference = root_retention_.issue();
        if (!reference && reference.status().code() == StatusCode::fault)
            return fail_locked(reference.status());
        if (reference) report_.published = true;
        return reference;
    }
    [[nodiscard]] Node *root()
    {
        Status authority{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (!admission()) return nullptr;
            // Publication is the final authority boundary. Keep the mount
            // gate held while checking the original Session so a completed
            // external abort cannot publish a stale root.
            authority = identity();
            if (authority)
            { report_.published = true; legacy_exposure_ = true; nodes_[0].borrowed_identity_exposed_ = true; return &nodes_[0]; }
            report_.phase = Ext4MountPhase::failed;
            report_.status = authority;
            root_retention_.stop_admission();
        }
        // Quarantine after releasing the mount lock; downstream completion
        // and cleanup paths may also need that lock.
        static_cast<void>(owner_.fail(authority));
        return nullptr;
    }
    [[nodiscard]] Ext4MountReport report() const { smp::ScopedSpinLock guard(lock_); return report_; }
    [[nodiscard]] MetadataOwnerReport buffers() const { return owner_.buffers(); }
    [[nodiscard]] Ext4StartupRecoveryReport startup_report() const { return owner_.startup_report(); }
    // Kernel diagnostics for the checked mounted identity; no device I/O.
    [[nodiscard]] Result<Ext4InodeLifetimeSnapshot> lifetime_snapshot(u32 inode) const
    {
        smp::ScopedSpinLock guard(lock_);
        for (const auto &node : nodes_)
            if (node.inode_ == inode) return lifetime_.snapshot(node.lifetime_key_);
        return Status::not_found("mounted inode has no runtime record");
    }
    // Trusted caller-supplied metadata is confined to the unqualified policy.
    [[nodiscard]] Status update_metadata(Ext4InodeMetadataUpdate update)
    {
        auto epoch = begin_update(Ext4MountedPolicy::maintenance_only);
        return epoch ? commit_update(update, epoch.value(), mode_regular) : epoch.status();
    }
    [[nodiscard]] Status sync()
    {
        return read_operation([&] { return owner_.bound_device().flush(); }, true);
    }
    [[nodiscard]] Status sync_persistent(const MountedNode &node)
    {
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
            return Status::unsupported("persistent sync requires ordered-data journalled mounted storage");
        // Authenticate the original Node and inode generation while retaining
        // its I/O lease. The provider's serialized mutations already complete
        // their journal protocol; success here also requires device FLUSH.
        return read_operation([&] { return owner_.bound_device().flush(); }, true, nullptr, &node, true);
    }
    [[nodiscard]] Status close()
    {
        if (auto status = begin_drain(); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = drained_locked(); !status) return status;
            report_.phase = Ext4MountPhase::updating;
        }
        auto status = owner_.close(); if (!status) return fail(status);
        smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; return {};
    }
    [[nodiscard]] Status begin_drain()
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = retention_integrity_locked(); !status) return status;
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::draining) return {};
        if (auto status = admission(); !status) return status;
        if (legacy_exposure_) return Status::busy("mounted provider has permanent borrowed exposure");
        if (report_.reading) return Status::busy("mounted reader in flight");
        if (auto status = orphan_work_drained_locked(); !status) return status;
        if (auto status = root_retention_.empty(); !status) return status;
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight || buffers.transaction_active)
            return Status::busy("mounted buffers have retained work");
        if (auto status = identity(); !status) return fail_locked(status);
        if (auto status = root_retention_.begin_drain(); !status) return status;
        report_.phase = Ext4MountPhase::draining; return {};
    }
    // One-way shutdown after the VFS has drained every retained node. A normal
    // file sync deliberately leaves needs_recovery set while mounted.
    [[nodiscard]] Status freeze_clean()
    {
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
            return Status::unsupported("clean freeze requires mounted-dirty policy");
        if (auto status = begin_drain(); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = drained_locked(); !status) return status;
            report_.phase = Ext4MountPhase::freezing;
        }
        if (auto status = identity(); !status) return fail(status);
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(true, true);
        if (auto status = inspection_.prepare(owner_.bound_device(), owner_.bounds_); !status) return fail(status);
        if (!inspection_.binding()->needs_recovery || !inspection_.report().empty_log)
            return fail(Status::fault("clean freeze requires dirty empty journal"));
        auto &device = owner_.bound_device();
        if (auto status = device.flush(); !status) return fail(status);
        if (auto status = device.write_blocks(u64{binding_.layout.journal_first_block} * 2,
                std::as_bytes(std::span{inspection_.journal_})); !status) return fail(status);
        if (auto status = device.flush(); !status) return fail(status);
        auto super = inspection_.filesystem_;
        Ext4RecoveryCompletion<8, 2>::le32(super, 96, ext4_checked::le32(super, 96) & ~u32{4});
        Ext4RecoveryCompletion<8, 2>::le32(super, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        if (auto status = device.write_blocks(2, std::as_bytes(std::span{super})); !status) return fail(status);
        if (auto status = device.flush(); !status) return fail(status);
        reset(view_);
        if (auto status = owner_.close(); !status) return fail(status);
        smp::ScopedSpinLock guard(lock_);
        report_.phase = Ext4MountPhase::clean_sealed;
        return {};
    }

  private:
    [[nodiscard]] Status mount_common_storage(Registry &registry, typename Registry::Registration key,
                                               block::SessionBounds bounds)
    {
        auto &storage = *common_storage_;
        const auto one = reinterpret_cast<usize>(&storage), two = reinterpret_cast<usize>(this);
        if ((one <= two ? two - one < sizeof(storage) : one - two < sizeof(*this)) ||
            startup_workspace_ != &storage.startup_ || reclaim_workspace_ != &storage.reclaim_)
            return Status::invalid_argument("common storage overlaps Mount or differs from its original domains");
        {
            smp::ScopedSpinLock guard(storage.lock_);
            if (storage.claimant_ || storage.operation_ || storage.publication_ || storage.requests_)
                return Status::busy("common storage already belongs to a Mount lifetime");
            storage.claimant_ = this;
        }
        // Permanent binding precedes construction and transport. This lock is
        // never held across Owner/cache/Mount locks or an operation callback.
        storage.requests_.emplace(*this);
        return common_mount_scope(registry, key, bounds, storage.child_,
            [&](const TypedColdOpening &original) {
                storage.operation_.emplace(*this, original, storage.child_);
                storage.publication_.emplace(*storage.operation_);
                const auto status = typed_run_common_operation(*storage.operation_, *storage.publication_);
                storage.publication_.reset();
                storage.operation_.reset();
                return status;
            });
    }
    [[nodiscard]] Status mount_after_startup()
    {
        if (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime &&
            (!startup_workspace_ || owner_.startup_ordinary_selected()))
            if (auto status = recover_and_mark_dirty(); !status) return fail(status);
        auto status = inspect(false);
        if (status) status = view_.open(reader_, binding_);
        if (status && policy_ != Ext4MountedPolicy::maintenance_only) status = view_.validate_stored_metadata();
        if (!status)
        {
            // The retained common opening owns its original failure/quarantine
            // path through scope termination, including decoder refusals.
            if (owner_.common_opening_) return fail(status);
            if (status.code() != StatusCode::unsupported && status.code() != StatusCode::invalid_argument &&
                status.code() != StatusCode::no_memory) return fail(status);
            if (auto closed = owner_.close(); !closed) return fail(closed);
            smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status;
        }
        if (startup_workspace_)
            if (auto admitted = owner_.startup_admit(); !admitted) return fail(admitted);
        if (auto root = node(2, "/"); !root) return fail(root.status());
        if (auto status = root_retention_.bind(&nodes_[0], this, identity_.session_serial,
                                              identity_.registration_generation); !status) return fail(status);
        smp::ScopedSpinLock guard(lock_); report_.epoch = 1; report_.phase = Ext4MountPhase::readable; return {};
    }
    friend Owner;
    [[nodiscard]] Status startup_opening(const Owner *owner, u64 epoch) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (owner != &owner_ || epoch != 1 || report_.phase != Ext4MountPhase::opening ||
            report_.epoch || report_.published || report_.reading)
            return Status::fault("startup mount opening publication domain differs");
        for (const auto &node : nodes_)
            if (node.inode_) return Status::fault("startup runtime Node was populated before admission");
        return {};
    }
    [[nodiscard]] Status reserve_name_publication(u32 inode) const
    {
        smp::ScopedSpinLock guard(lock_);
        for (const auto &node : nodes_)
            if (node.inode_ == inode && node.rename_count_ == node.rename_names_.size())
                return Status::no_memory("mounted immutable name history capacity");
        return {};
    }
    // Mount lock held; every previous slot remains byte-exact for retained or
    // raw callers. Capacity is checked before the namespace writer issues I/O.
    [[nodiscard]] Status publish_name(u32 inode, const FixedString<max_path_segment> &name)
    {
        for (auto &node : nodes_)
            if (node.inode_ == inode)
            {
                if (node.rename_count_ == node.rename_names_.size())
                    return Status::fault("mounted reserved name publication capacity changed");
                node.rename_names_[node.rename_count_] = name; ++node.rename_count_;
            }
        return {};
    }
    // Mount lock held. A detached identity may acquire another hold only from
    // a real retained descriptor; public keys and diagnostic snapshots confer
    // no authority to resurrect a final inode.
    [[nodiscard]] Result<Lease> retain_existing(Ext4InodeLifetimeKey key, Ext4InodeHold kind)
    {
        if (key.mount != this || key.session != identity_.session_serial)
            return Status::bad_descriptor("mounted hold identity is foreign");
        auto fresh = lifetime_.retain(key, kind);
        if (fresh || fresh.status().code() != StatusCode::busy) return fresh;
        for (const auto &held : reference_holds_)
            if (held.node && held.lease && held.node->lifetime_key_ == key)
                return lifetime_.clone(*held.lease, kind);
        return fresh.status();
    }

    [[nodiscard]] Status namespace_regular(MountedNode &parent, std::string_view old_name,
                                           std::string_view new_name, bool directory, bool replace)
    {
        if (!reclaim_workspace_) return unsupported();
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch); };
        if (directory) return reject(Status::unsupported("mounted namespace removal requires regular file"));
        if (parent.mount_ != this) return reject(Status::bad_descriptor("mounted namespace parent is foreign"));
        const auto valid_name = [](std::string_view name) {
            return !name.empty() && name != "." && name != ".." &&
                   name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
        };
        if (!valid_name(old_name) || (replace && !valid_name(new_name)))
            return reject(Status::invalid_argument("invalid mounted namespace name"));
        auto checked_parent = view_.inode(parent.inode_);
        if (!checked_parent) return reject(checked_parent.status());
        if (parent.lifetime_key_ != Ext4InodeLifetimeKey{this, identity_.session_serial,
                                                       parent.inode_, checked_parent.value().generation})
            return fail(Status::fault("mounted namespace parent generation changed"));
        auto source = view_.lookup(parent.inode_, old_name);
        if (!source) return reject(source.status());
        auto source_inode = view_.inode(source.value());
        if (!source_inode) return reject(source_inode.status());
        auto source_metadata = metadata(source.value());
        if (!source_metadata) return reject(source_metadata.status());
        if (source_metadata.value().type != NodeType::regular || source.value() < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted namespace source requires ordinary regular inode"));
        u32 removed = source.value(), removed_generation = source_inode.value().generation;
        if (replace)
        {
            auto target = view_.lookup(parent.inode_, new_name);
            if (!target) return reject(target.status());
            if (target.value() == source.value())
            {
                auto directory_metadata = metadata(parent.inode_);
                if (!directory_metadata) return reject(directory_metadata.status());
                if (directory_metadata.value().type != NodeType::directory ||
                    !directory_metadata.value().link_count || (directory_metadata.value().mode & 0300) != 0300)
                    return reject(Status::unsupported("mounted rename parent lacks owner write/search bits"));
                if (old_name != new_name && source_metadata.value().link_count < 2)
                    return reject(Status::io_error("mounted same-inode rename links disagree"));
                return reject_unprepared({}, epoch);
            }
            auto target_inode = view_.inode(target.value());
            if (!target_inode) return reject(target_inode.status());
            removed = target.value(); removed_generation = target_inode.value().generation;
        }
        auto victim_metadata = metadata(removed);
        if (!victim_metadata) return reject(victim_metadata.status());
        if (victim_metadata.value().type != NodeType::regular || !victim_metadata.value().link_count ||
            removed < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted namespace victim requires live ordinary regular inode"));
        // Reserve/observe the actual victim address before durable publication.
        // It is never raw-exposed by this internal lookup.
        auto victim_result = node(removed, replace ? new_name : old_name);
        if (!victim_result) return reject(victim_result.status());
        auto *victim = static_cast<MountedNode *>(victim_result.value());
        const Ext4InodeLifetimeKey key{this, identity_.session_serial, removed, removed_generation};
        if (victim->lifetime_key_ != key) return fail(Status::fault("mounted namespace victim generation changed"));
        FixedString<max_path_segment> replacement;
        if (replace)
            if (auto status = replacement.assign(new_name); !status) return reject(status);
        if (replace)
            if (auto status = reserve_name_publication(source.value()); !status) return reject(status);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        const Ext4NamespaceUpdate update{
            replace ? Ext4NamespaceOperation::replace : Ext4NamespaceOperation::unlink,
            parent.inode_, source.value(), replace ? removed : 0,
            checked_parent.value().generation, source_inode.value().generation,
            replace ? removed_generation : 0, old_name, new_name, seconds, nanoseconds};
        auto status = owner_.prepare_namespace(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        const bool orphan = owner_.report().orphan_cleanup_pending;
        if (orphan != (victim_metadata.value().link_count == 1))
            return fail(Status::fault("mounted namespace orphan decision disagrees with checked links"));
        if (orphan)
        {
            auto retained = owner_.retained_orphan_binding(key);
            if (!retained) return fail(retained.status());
            binding_ = retained.value();
            if (status = view_.open_retained_orphan(reader_, binding_, removed, removed_generation, 0); !status)
                return fail(status);
        }
        else
        {
            if (status = inspect(true); !status) return fail(status);
            if (status = view_.open(reader_, binding_); !status) return fail(status);
        }
        if (view_.lookup(parent.inode_, old_name).status().code() != StatusCode::not_found)
            return fail(Status::fault("mounted namespace old name remains visible"));
        if (replace)
        {
            auto target = view_.lookup(parent.inode_, new_name);
            if (!target || target.value() != source.value())
                return fail(Status::fault("mounted replacement namespace disagrees"));
        }
        auto changed = metadata(removed);
        if (!changed || changed.value().link_count != victim_metadata.value().link_count - 1)
            return fail(changed ? Status::fault("mounted namespace victim links disagree") : changed.status());
        if (!orphan)
            if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating || pending_orphan_)
                status = Status::fault("mounted namespace publication epoch changed");
            else
            {
                if (orphan)
                {
                    status = lifetime_.mark_orphan(key);
                    if (status)
                    { pending_orphan_ = victim; orphan_seconds_ = seconds; orphan_nanoseconds_ = nanoseconds; }
                }
                if (status)
                {
                    if (replace) status = publish_name(source.value(), replacement);
                    if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
                }
            }
        }
        if (!status) return fail(status);
        return drain_pending_orphan();
    }

    [[nodiscard]] Status drain_pending_orphan()
    {
        MountedNode *victim{}; Ext4InodeLifetimeKey key{}; u64 epoch{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = retention_integrity_locked(); !status) return status;
            const auto mutation = lifetime_.node_mutation_active(this, identity_.session_serial);
            if (!mutation) return fail_locked(mutation.status());
            // Descriptor release is legal during a mutation. Deferred orphan
            // transport cannot start until the original I/O slot is consumed.
            if (mutation.value()) return {};
            if (!pending_orphan_ || report_.phase != Ext4MountPhase::readable || report_.reading) return {};
            victim = pending_orphan_; key = victim->lifetime_key_;
            if (victim->references_ || victim->borrowed_identity_exposed_) return {};
            if (!reclaim_workspace_ || reclaim_workspace_->claimant != this ||
                key.mount != this || key.session != identity_.session_serial || key.inode != victim->inode_)
                return Status::fault("mounted orphan workspace or identity changed");
            auto state = lifetime_.snapshot(key);
            if (!state) return state.status();
            if (state.value().total()) return {};
            if (state.value().phase != Ext4InodeLifetimePhase::ready)
                return Status::fault("mounted orphan is not ready for exact dispatch");
            if (report_.epoch == ~u64{0}) return Status::overflow("mounted reclaim epoch exhausted");
            epoch = report_.epoch; report_.phase = Ext4MountPhase::updating;
        }
        owner_.publication_domain_ = epoch;
        auto status = identity(epoch);
        if (!status) return fail(status);
        // Scheduling grants a checked dispatch, never an acknowledgement. The
        // same Owner privately claims it immediately before journal publication.
        status = lifetime_.schedule_exact(key, [&](const typename Lifetime::Ticket &ticket) -> Status {
            if (auto checked = validate_node_retirement(ticket); !checked) return checked;
            auto &workspace = *reclaim_workspace_;
            reset(view_);
            auto prepared = owner_.prepare_orphan_reclaim(lifetime_, ticket, key,
                workspace.original, workspace.preparation, orphan_seconds_, orphan_nanoseconds_);
            if (!prepared) return prepared;
            if (auto published = owner_.publish_orphan_reclaim(workspace.observed, workspace.verification); !published)
                return published;
            return owner_.checkpoint_orphan_reclaim(workspace.observed, workspace.verification);
        });
        if (!status) return fail(status);
        if (!owner_.report().orphan_reclaimed || owner_.report().orphan_cleanup_pending)
            return fail(Status::fault("mounted reclaim did not durably complete"));
        if (status = identity(epoch); !status) return fail(status);
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating ||
                pending_orphan_ != victim || victim->lifetime_key_ != key ||
                victim->references_ || victim->borrowed_identity_exposed_)
                status = Status::fault("mounted reclaim retirement identity changed");
            else
            {
                victim->inode_ = 0; victim->lifetime_key_ = {}; victim->name_ = {};
                victim->rename_names_ = {}; victim->rename_count_ = 0;
                pending_orphan_ = nullptr; orphan_seconds_ = orphan_nanoseconds_ = 0;
                ++report_.epoch; report_.phase = Ext4MountPhase::readable;
            }
        }
        return status ? status : fail(status);
    }

    [[nodiscard]] Result<Metadata> metadata(u32 inode)
    {
        if (policy_ == Ext4MountedPolicy::maintenance_only) return view_.query_metadata(inode);
        auto result = view_.query_stored_metadata(inode);
        if (result)
        {
            auto &a = result.value().attributes;
            a.access.source = a.modification.source = a.change.source = TimestampSource::maintained_ext4;
        }
        return result;
    }
    // Mount lock held. The caller closes admission only after obtaining any
    // required lifetime hold; this check itself performs no I/O or mutation.
    [[nodiscard]] Result<u64> update_admission_locked(Ext4MountedPolicy required)
    {
        if (auto status = admission(); !status) return status;
        if (policy_ != required) return Status::unsupported("mounted mutation policy unavailable");
        if (pending_orphan_) return Status::busy("mounted orphan cleanup precedes another mutation");
        if (report_.reading) return Status::busy("mounted reader in flight");
        if (report_.epoch == ~u64{0}) return Status::overflow("mounted admission epoch exhausted");
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight) return Status::busy("mounted canonical reader in flight");
        return report_.epoch;
    }
    [[nodiscard]] Result<u64> begin_update(Ext4MountedPolicy required)
    {
        u64 epoch;
        {
            smp::ScopedSpinLock guard(lock_);
            auto admitted = update_admission_locked(required);
            if (!admitted) return admitted.status();
            epoch = admitted.value(); report_.phase = Ext4MountPhase::updating;
        }
        owner_.publication_domain_ = epoch;
        if (auto status = identity(epoch); !status) return fail(status);
        return epoch;
    }
    // The owner has not entered prepare: rearm is invalid in its open phase.
    [[nodiscard]] Status reject_unprepared(Status status, u64 epoch,
                                           const NodeMutationContext *original = nullptr)
    {
        if (!view_.valid()) return fail(status);
        if (auto authority = identity(epoch, original); !authority) return fail(authority);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4MountPhase::updating || report_.epoch != epoch)
                status = Status::fault("mounted rejection epoch changed");
            else { if (!original && !mutation_context_) report_.phase = Ext4MountPhase::readable; return status; }
        }
        return fail(status);
    }
    [[nodiscard]] Status ordered_clock(u32 &seconds, u32 &nanoseconds)
    {
        auto sample = arch::realtime_sample();
        if (!sample) return Status::io_error("ordered data requires active realtime");
        const auto &value = sample.value();
        if (value.value.nanoseconds >= time::nanoseconds_per_second || !value.wall_generation ||
            !value.monotonic.generation ||
            (value.origin != time::RealtimeOrigin::boot_rtc &&
             value.origin != time::RealtimeOrigin::administrator_software) ||
            value.value.seconds < 0 || value.value.seconds > 0x7fffffffLL)
            return Status::unsupported("ordered data realtime sample outside profile");
        seconds = static_cast<u32>(value.value.seconds);
        nanoseconds = value.value.nanoseconds;
        return {};
    }
    [[nodiscard]] Status data_image(u32 inode, u32 logical, u32 &physical, std::array<u8, 1024> &image)
    {
        auto block = view_.unique_initialized_data_block(inode, logical);
        if (!block) return block.status();
        physical = block.value();
        typename Owner::Pin pin;
        if (auto status = owner_.pin(physical, pin); !status) return fail(status);
        auto status = pin.read(0, std::as_writable_bytes(std::span{image}));
        const auto released = pin.release();
        return status ? released : status;
    }
    // Mount lock held. A public inode/key or a copied Node is never authority
    // for a synchronous mutation. Validate its original slot before its fields.
    [[nodiscard]] Status authentic_node_locked(const MountedNode &node) const
    {
        bool member{};
        for (const auto &slot : nodes_) if (&slot == &node) member = true;
        if (!member) return Status::bad_descriptor("mounted mutation Node is not an original slot");
        const auto &key = node.lifetime_key_;
        if (node.mount_ != this || !node.inode_ || key.mount != this ||
            key.session != identity_.session_serial || key.inode != node.inode_)
            return Status::bad_descriptor("mounted mutation Node identity differs");
        for (const auto &slot : nodes_)
            if (&slot != &node && slot.inode_ == node.inode_)
                return Status::bad_descriptor("mounted mutation Node identity is duplicated");
        auto state = lifetime_.snapshot(key);
        return state ? Status{} : Status::bad_descriptor("mounted mutation Node lifetime is absent");
    }
    // Strong fd admission requires an original retained live namespace node.
    // A raw pointer, diagnostic count or detached inode is not this contract.
    [[nodiscard]] Status persistent_node_locked(const MountedNode &node) const
    {
        if (auto status = authentic_node_locked(node); !status) return status;
        const auto state = lifetime_.snapshot(node.lifetime_key_);
        if (!state || state.value().phase != Ext4InodeLifetimePhase::linked)
            return Status::bad_descriptor("persistent sync node is detached");
        for (const auto &held : reference_holds_)
            if (held.node == &node && held.lease && held.lease->valid() &&
                lifetime_.validate_lease(*held.lease, node.lifetime_key_, Ext4InodeHold::descriptor))
                return {};
        return Status::bad_descriptor("persistent sync node has no live retained descriptor");
    }
    // Mount lock held. The operation's original address-bound Lease remains
    // authoritative even if its diagnostic count or Node fields are changed.
    [[nodiscard]] Status mutation_integrity_locked(const NodeMutationContext *original = nullptr) const
    {
        if (original)
        {
            if (mutation_context_ != original || mutation_node_ != &original->node ||
                mutation_key_ != original->key || mutation_lease_ != &original->lease ||
                original->epoch != owner_.publication_domain_)
                return Status::fault("mounted original mutation binding changed");
            if (auto status = authentic_node_locked(original->node); !status) return status;
            if (original->node.lifetime_key_ != original->key)
                return Status::fault("mounted original mutation Node changed");
            if (auto status = lifetime_.validate_node_mutation(original->lease, original->key,
                                                              original, original->epoch); !status) return status;
            return lifetime_.validate_lease(original->lease, original->key, Ext4InodeHold::io);
        }
        if (mutation_context_) return mutation_integrity_locked(mutation_context_);
        if (!mutation_node_)
            return mutation_lease_ || mutation_key_ != Ext4InodeLifetimeKey{} ?
                Status::fault("mounted mutation binding is incomplete") : Status{};
        if (auto status = authentic_node_locked(*mutation_node_); !status) return status;
        if (mutation_node_->lifetime_key_ != mutation_key_ || !mutation_lease_)
            return Status::fault("mounted original mutation identity changed");
        return lifetime_.validate_lease(*mutation_lease_, mutation_key_, Ext4InodeHold::io);
    }
    // The verified view supplies the table location, but a resident canonical
    // table is not a new physical observation. Read through the original owned
    // Session and check the complete inode checksum before its generation.
    [[nodiscard]] Status physical_node_generation(Ext4InodeLifetimeKey key)
    {
        if (!view_.valid() || view_.layout_.block_size != 1024 || !view_.inodes_per_group_ ||
            !key.inode || key.inode > view_.inodes_)
            return Status::fault("mounted physical inode view is unavailable");
        const auto local = (key.inode - 1) % view_.inodes_per_group_;
        const auto table = view_.groups_[(key.inode - 1) / view_.inodes_per_group_].table + local / 4;
        std::array<u8, 1024> block{};
        if (auto status = owner_.bound_device().read_blocks(u64{table} * 2,
                std::as_writable_bytes(std::span{block})); !status) return status;
        const auto inode = std::span<const u8>{block}.subspan((local % 4) * 256, 256);
        if (auto status = ext4_checked::inode_checksum(view_.checksum_seed(), key.inode, inode); !status)
            return status;
        return ext4_checked::le32(inode, 100) == key.generation ? Status{} :
            Status::io_error("mounted physical inode generation changed");
    }
    enum class MutationTerminalVerification { wrapper, operation };
    template <MutationTerminalVerification Verification = MutationTerminalVerification::wrapper,
              bool CheckedMetadata = false, typename Operation>
    [[nodiscard]] Status node_mutation_operation(MountedNode &node, Operation operation)
    {
        std::optional<Lease> in_flight;
        std::optional<NodeMutationContext> original;
        Ext4InodeLifetimeKey key{};
        u64 epoch{};
        {
            smp::ScopedSpinLock guard(lock_);
            if constexpr (CheckedMetadata)
                if (policy_ == Ext4MountedPolicy::maintenance_only)
                    return Status::unsupported("checked mutation requires maintained metadata");
            auto admitted = update_admission_locked(CheckedMetadata ? policy_ :
                policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime ?
                Ext4MountedPolicy::ordered_data_dirty_noatime : Ext4MountedPolicy::ordered_data_noatime);
            if (!admitted) return admitted.status();
            if (auto status = authentic_node_locked(node); !status) return status;
            key = node.lifetime_key_;
            auto held = retain_existing(key, Ext4InodeHold::io);
            if (!held) return held.status();
            in_flight.emplace(std::move(held).value());
            mutation_node_ = &node; mutation_key_ = key; mutation_lease_ = &*in_flight;
            epoch = admitted.value(); report_.phase = Ext4MountPhase::updating;
            original.emplace(node, key, *in_flight, epoch); mutation_context_ = &*original;
            if (auto status = lifetime_.claim_node_mutation(*in_flight, key, &*original, epoch); !status)
            {
                const auto released = lifetime_.release(*in_flight);
                mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr; mutation_context_ = nullptr;
                return fail_locked(released ? status : released);
            }
        }
        owner_.publication_domain_ = epoch;
        auto status = identity(epoch, &*original);
        if (!status) status = fail(status);
        else
        {
            auto checked = view_.inode(key.inode);
            if (!checked)
                status = checked.status().code() == StatusCode::no_memory ||
                         checked.status().code() == StatusCode::overflow ?
                    reject_unprepared(checked.status(), epoch, &*original) : fail(checked.status());
            else if (checked.value().generation != key.generation)
                status = fail(Status::io_error("mounted inode generation changed during mutation"));
            else if (auto observed = physical_node_generation(key); !observed) status = fail(observed);
            else if (auto authority = identity(epoch, &*original); !authority) status = fail(authority);
            else status = operation(*original);
        }
        // A continuation that publishes names performs this physical check
        // inside its complete participant-lease scope before publication.
        if constexpr (Verification == MutationTerminalVerification::wrapper)
            if (status)
                if (auto observed = physical_node_generation(key); !observed) status = fail(observed);
        {
            smp::ScopedSpinLock guard(lock_);
            auto integrity = mutation_integrity_locked(&*original);
            if (report_.reading) integrity = Status::fault("mounted reader entered original mutation");
            if (mutation_node_ != &node || mutation_key_ != key || mutation_lease_ != &*in_flight)
                integrity = Status::fault("mounted original mutation binding changed");
            // Always consume the actual local Lease, including quarantined exits.
            const auto released = lifetime_.release_node_mutation(*in_flight, original->key,
                                                                  &*original, original->epoch);
            mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr;
            mutation_context_ = nullptr;
            if (!integrity || !released) status = fail_locked(!integrity ? integrity : released);
            else if (report_.phase != Ext4MountPhase::failed)
            {
                if (report_.phase != Ext4MountPhase::updating ||
                    (report_.epoch != epoch && report_.epoch != epoch + 1))
                    status = fail_locked(Status::fault("mounted mutation terminal epoch changed"));
                else report_.phase = Ext4MountPhase::readable;
            }
        }
        const auto cleanup = drain_pending_orphan();
        return cleanup ? status : cleanup;
    }
    [[nodiscard]] ModeChangeResult change_owner(MountedNode &node, u32 uid, u32 gid, user::Credentials caller)
    { return change_metadata<true>(node, uid, gid, caller); }
    template <bool Ownership>
    [[nodiscard]] ModeChangeResult change_metadata(MountedNode &node, u32 first, u32 second, user::Credentials caller)
    {
        if (Ownership && caller.euid != user::root_uid)
            return ModeChangeResult::failure(Status::permission("mounted ownership change requires root"));
        const auto status = node_mutation_operation<MutationTerminalVerification::wrapper, true>(node,
            [&](const NodeMutationContext &original) -> Status {
                const auto rejected = [&](Status value) { return reject_unprepared(value, original.epoch, &original); };
                auto current = metadata(original.key.inode);
                if (!current) return rejected(current.status());
                const auto &value = current.value();
                if ((value.type != NodeType::regular && value.type != NodeType::directory) ||
                    original.key.inode < view_.first_ordinary_inode())
                    return rejected(Status::unsupported("mounted metadata mutation requires ordinary regular inode or directory"));
                u32 uid = value.uid, gid = value.gid, permissions{};
                if constexpr (Ownership)
                {
                    const auto decision = owner_change_policy(value.uid, value.gid, value.mode, first, second, caller);
                    if (!decision.authorized) return rejected(Status::permission("mounted ownership change requires root"));
                    uid = decision.uid; gid = decision.gid; permissions = decision.permissions;
                }
                else
                {
                    const auto decision = mode_change_policy(value.uid, value.gid, first, caller);
                    if (!decision.authorized) return rejected(Status::permission("mounted mode requires owner or root"));
                    permissions = decision.permissions;
                }
                auto sampled = arch::realtime_sample();
                if (!sampled) return rejected(Status::io_error("maintained metadata requires active realtime"));
                const auto &clock = sampled.value();
                if (clock.value.nanoseconds >= time::nanoseconds_per_second || !clock.wall_generation ||
                    !clock.monotonic.generation || (clock.origin != time::RealtimeOrigin::boot_rtc &&
                    clock.origin != time::RealtimeOrigin::administrator_software))
                    return rejected(Status::io_error("invalid maintained metadata realtime sample"));
                if (clock.value.seconds < 0 || clock.value.seconds > 0x7fffffffLL)
                    return rejected(Status::overflow("journalled metadata ctime outside bounded writer range"));
                Ext4InodeMetadataUpdate update{original.key.inode, uid, gid,
                    static_cast<u32>(clock.value.seconds), clock.value.nanoseconds,
                    static_cast<u16>(permissions)};
                return commit_update<Ownership, !Ownership>(update, original.epoch, value.mode & mode_type_mask,
                    0, nullptr, 0, false, 0, &original);
            });
        return status.code() == StatusCode::unsupported && report().phase != Ext4MountPhase::failed ?
            ModeChangeResult::unavailable() : ModeChangeResult::failure(status);
    }
    [[nodiscard]] Status write_regular(MountedNode &node, usize offset, std::span<const std::byte> input)
    {
        if (input.empty()) return read_operation([&] { return Status{}; }, false, nullptr, &node);
        if (common_storage_)
        {
            const auto current = node.query_metadata();
            if (!current) return current.status();
            // Observation selects a route; the typed operation authenticates
            // the actual original Node/Lease and current media before writing.
            if (current.value().type == NodeType::regular && !current.value().size && !current.value().blocks)
                return typed_private_refill(node, common_storage_->child_, *common_storage_->requests_, offset, input);
        }
        return node_mutation_operation(node, [&](const NodeMutationContext &original) {
            return write_regular_admitted(original, offset, input);
        });
    }
    [[nodiscard]] Status write_regular_admitted(const NodeMutationContext &original, usize offset,
                                                std::span<const std::byte> input)
    {
        const auto inode = original.key.inode; const auto epoch = original.epoch;
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch, &original); };
        auto current = metadata(inode);
        if (!current) return reject(current.status());
        const auto &m = current.value();
        if constexpr (sizeof(usize) > sizeof(u32))
            if (offset > 0xffffffffULL)
                return reject(Status::unsupported("ordered data write offset outside 32-bit inode size"));
        if (m.type != NodeType::regular || inode < view_.first_ordinary_inode() ||
            (m.mode & 06000) || input.size() > 1024 ||
            input.size() > 0xffffffffULL - offset)
            return reject(Status::unsupported("ordered data write target or range"));
        const u64 end = u64{offset} + input.size();
        if (!m.size)
        {
            if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime || end > 1024 || m.blocks)
                return reject(Status::unsupported("first data write requires dirty empty single-block file"));
            std::array<u8, 1024> image{};
            for (usize index = 0; index < input.size(); ++index)
                image[offset + index] = std::to_integer<u8>(input[index]);
            u32 seconds{}, nanoseconds{};
            if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
            const Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
                static_cast<u16>(m.mode & mode_permission_mask), true,
                static_cast<u32>(end), seconds, nanoseconds};
            return commit_update(update, epoch, mode_regular, 0, &image, u32(offset), true, 0, &original);
        }
        if (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime &&
            m.size >= 1024 && m.size <= 15360 && !(m.size % 1024) &&
            offset >= m.size && end <= u64{m.size} + 1024)
        {
            std::array<u8, 1024> image{};
            for (usize index = 0; index < input.size(); ++index)
                image[offset - m.size + index] = std::to_integer<u8>(input[index]);
            u32 seconds{}, nanoseconds{};
            if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
            const Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
                static_cast<u16>(m.mode & mode_permission_mask), true,
                static_cast<u32>(end), seconds, nanoseconds};
            return commit_update(update, epoch, mode_regular, 0, &image, u32(offset), false, m.size, &original);
        }
        const u32 logical = static_cast<u32>(offset / 1024);
        if (logical != (end - 1) / 1024 || end > ((u64{m.size} + 1023) / 1024) * 1024)
            return reject(Status::unsupported("ordered data write crosses an allocated block"));
        std::array<u8, 1024> image{}; u32 physical{};
        if (auto status = data_image(inode, logical, physical, image); !status) return fail(status);
        if (end > m.size)
            for (u64 at = m.size; at < offset; ++at)
                image[at % 1024] = 0;
        for (usize index = 0; index < input.size(); ++index)
            image[offset % 1024 + index] = std::to_integer<u8>(input[index]);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
            static_cast<u16>(m.mode & mode_permission_mask), true,
            static_cast<u32>(end > m.size ? end : m.size), seconds, nanoseconds};
        return commit_update(update, epoch, mode_regular, physical, &image, 0, false, 0, &original);
    }
    [[nodiscard]] Status truncate_regular(MountedNode &node, usize size)
    {
        if (common_storage_ && !size)
            return typed_private_zero(node, common_storage_->child_, *common_storage_->requests_, size);
        return node_mutation_operation(node, [&](const NodeMutationContext &original) {
            return truncate_regular_admitted(original, size);
        });
    }
    [[nodiscard]] Status truncate_regular_admitted(const NodeMutationContext &original, usize size)
    {
        const auto inode = original.key.inode; const auto epoch = original.epoch;
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch, &original); };
        auto current = metadata(inode);
        if (!current) return reject(current.status());
        const auto &m = current.value();
        if constexpr (sizeof(usize) > sizeof(u32))
            if (size > 0xffffffffULL)
                return reject(Status::unsupported("ordered data size outside 32-bit inode size"));
        if (m.type != NodeType::regular || !m.size || !size ||
            inode < view_.first_ordinary_inode() || (m.mode & 06000) ||
            (u64{size} + 1023) / 1024 != (u64{m.size} + 1023) / 1024)
            return reject(Status::unsupported("ordered data resize changes allocated block count"));
        if (size == m.size) return reject({});
        const u32 logical = static_cast<u32>((m.size - 1) / 1024);
        std::array<u8, 1024> image{}; u32 physical{};
        if (auto status = data_image(inode, logical, physical, image); !status) return fail(status);
        const usize zero_start = (size < m.size ? size : m.size) % 1024;
        for (usize at = zero_start; at < image.size(); ++at) image[at] = 0;
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
            static_cast<u16>(m.mode & mode_permission_mask), true,
            static_cast<u32>(size), seconds, nanoseconds};
        return commit_update(update, epoch, mode_regular, physical, &image, 0, false, 0, &original);
    }
    [[nodiscard]] Result<Node *> create_node(MountedNode &parent, std::string_view name,
                                                 const InitialNodeAttributes &attributes, bool retained_result)
    {
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) -> Result<Node *> { return reject_unprepared(status, epoch); };
        if (attributes.type != NodeType::regular && attributes.type != NodeType::directory)
            return reject(Status::unsupported("mounted creation requires a regular file or bounded directory"));
        if (attributes.mode & ~mode_permission_mask)
            return reject(Status::invalid_argument("mounted creation permissions outside POSIX mode"));
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos ||
            name.find('\0') != std::string_view::npos)
            return reject(Status::invalid_argument("invalid mounted creation name"));
        auto current = metadata(parent.inode_);
        if (!current) return reject(current.status());
        if (current.value().type != NodeType::directory || !current.value().link_count)
            return reject(Status::not_directory("mounted creation parent is not a live directory"));
        auto prior = view_.lookup(parent.inode_, name);
        if (prior) return reject(Status::already_exists("mounted creation name exists"));
        if (prior.status().code() != StatusCode::not_found) return reject(prior.status());

        // Reserve a stable result address before the writer can issue a write or
        // FLUSH. An unpublished slot has inode zero and cannot be found by node().
        MountedNode *reserved = nullptr;
        for (auto &candidate : nodes_)
            if (!candidate.inode_ && !candidate.borrowed_identity_exposed_) { reserved = &candidate; break; }
        if (!reserved) { smp::ScopedSpinLock guard(lock_); reserved = evict_locked(); }
        if (!reserved) return reject(Status::no_memory("mounted metadata node capacity"));
        if (auto status = reserved->name_.assign(name); !status) return reject(status);
        const auto release = [&] { reserved->name_ = {}; };
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status)
        { release(); return reject(status); }
        const Ext4CreateUpdate update{parent.inode_, seconds, nanoseconds, name, attributes};
        auto status = owner_.prepare_create(update);
        if (!status)
        {
            release();
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        const u32 inode = owner_.created_inode();
        if (!inode || inode < view_.first_ordinary_inode())
            return fail(Status::fault("mounted creation did not reserve an ordinary inode"));
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status)
                return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted create checkpoint image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (status = view_.validate_stored_metadata(); !status) return fail(status);
        auto found = view_.lookup(parent.inode_, name);
        if (!found || found.value() != inode)
            return fail(Status::fault("mounted create namespace disagreement"));
        auto created = metadata(inode);
        const bool directory_created = attributes.type == NodeType::directory;
        if (!created || created.value().type != attributes.type ||
            created.value().size != (directory_created ? 1024U : 0U) ||
            created.value().blocks != (directory_created ? 2U : 0U) ||
            created.value().mode != ((directory_created ? mode_directory : mode_regular) | attributes.mode) ||
            created.value().uid != attributes.uid || created.value().gid != attributes.gid ||
            created.value().link_count != (directory_created ? 2U : 1U) ||
            created.value().attributes.change.seconds != seconds ||
            created.value().attributes.change.nanoseconds != nanoseconds ||
            created.value().attributes.modification.seconds != seconds ||
            created.value().attributes.modification.nanoseconds != nanoseconds)
            return fail(created ? Status::fault("mounted create inode disagreement") : created.status());
        auto changed_parent = metadata(parent.inode_);
        if (!changed_parent || changed_parent.value().link_count != current.value().link_count +
                (directory_created ? 1U : 0U) ||
            changed_parent.value().attributes.change.seconds != seconds ||
            changed_parent.value().attributes.change.nanoseconds != nanoseconds ||
            changed_parent.value().attributes.modification.seconds != seconds ||
            changed_parent.value().attributes.modification.nanoseconds != nanoseconds)
            return fail(changed_parent ? Status::fault("mounted create parent timestamp disagreement") :
                                          changed_parent.status());
        if (directory_created)
        {
            const auto dot = view_.lookup(inode, "."), dotdot = view_.lookup(inode, "..");
            if (!dot || !dotdot || dot.value() != inode || dotdot.value() != parent.inode_)
                return fail(Status::fault("mounted mkdir dot identity disagreement"));
        }
        auto checked_inode = view_.inode(inode);
        if (!checked_inode) return fail(checked_inode.status());
        if (status = owner_.directory_continuation_end(); !status) return fail(status);
        if (status = owner_.rearm(); !status) return fail(status);
        const Ext4InodeLifetimeKey lifetime_key{this, identity_.session_serial,
                                                inode, checked_inode.value().generation};
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating || reserved->inode_)
                status = Status::fault("mounted create publication epoch changed");
            else
            {
                status = lifetime_.observe(lifetime_key);
                if (status)
                {
                    reserved->mount_ = this; reserved->inode_ = inode;
                    reserved->lifetime_key_ = lifetime_key;
                    if (retained_result) status = reserved->retain_locked();
                    else reserved->borrowed_identity_exposed_ = true;
                    if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
                }
            }
        }
        return status ? Result<Node *>{reserved} : Result<Node *>{fail(status)};
    }
#include "ok/fs/ext4/detail/mounted_regular_move.inc"
    [[nodiscard]] Status rename_same_parent(MountedNode &parent, std::string_view old_name,
                                        Node &destination, std::string_view new_name)
    {
        if (&parent != &destination || parent.mount_ != this)
            return unsupported();
        // The namespace family supplies last-link replacement/orphan semantics.
        // Its no-target case retains the already qualified ordinary rename.
        if (reclaim_workspace_)
        {
            auto found = parent.lookup_child_retained(new_name);
            if (found)
            {
                auto released = found.value()->release();
                if (!released) return released;
                return namespace_regular(parent, old_name, new_name, false, true);
            }
            if (found.status().code() != StatusCode::not_found) return found.status();
        }
        FixedString<max_path_segment> replacement;
        if (auto status = replacement.assign(new_name); !status) return status;
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch); };
        auto source = view_.lookup(parent.inode_, old_name);
        if (!source) return reject(source.status());
        auto current = metadata(source.value());
        if (!current) return reject(current.status());
        if ((current.value().type != NodeType::regular && current.value().type != NodeType::directory) ||
            source.value() < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted rename requires ordinary file or directory source"));
        auto target = view_.lookup(parent.inode_, new_name);
        if (!target && target.status().code() != StatusCode::not_found)
            return reject(target.status());
        if (target && target.value() == source.value())
        {
            auto directory = metadata(parent.inode_);
            if (!directory) return reject(directory.status());
            if (directory.value().type != NodeType::directory || !directory.value().link_count ||
                (directory.value().mode & 0300) != 0300)
                return reject(Status::unsupported("mounted rename parent lacks owner write/search bits"));
            if (old_name != new_name && current.value().link_count < 2)
                return reject(Status::io_error("mounted same-inode rename links disagree"));
            if (old_name != new_name && current.value().type == NodeType::directory)
                return reject(Status::io_error("mounted source directory has a namespace alias"));
            // Namespace identity is unchanged; keep the owner and epoch open.
            return reject_unprepared({}, epoch);
        }
        if (auto status = reserve_name_publication(source.value()); !status) return reject(status);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        const Ext4RenameUpdate update{parent.inode_, source.value(), seconds, nanoseconds,
                                       old_name, new_name, current.value().type == NodeType::directory};
        auto status = owner_.prepare_rename(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status)
                return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted rename checkpoint image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        auto found = view_.lookup(parent.inode_, new_name);
        if (!found || found.value() != source.value() ||
            view_.lookup(parent.inode_, old_name).status().code() != StatusCode::not_found)
            return fail(Status::fault("mounted rename namespace disagreement"));
        auto changed = metadata(source.value());
        if (!changed || changed.value().attributes.change.seconds != seconds ||
            changed.value().attributes.change.nanoseconds != nanoseconds)
            return fail(Status::fault("mounted rename inode timestamp disagreement"));
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating)
                status = Status::fault("mounted rename epoch changed");
            else
            {
                status = publish_name(source.value(), replacement);
                if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
            }
        }
        return status ? status : fail(status);
    }
    template <bool CheckedOwnership = false, bool CheckedMode = false>
    [[nodiscard]] Status commit_update(Ext4InodeMetadataUpdate update, u64 epoch, u32 expected_type,
                                       u32 data_block = 0, const std::array<u8, 1024> *data = nullptr,
                                       u32 first_offset = 0, bool allocate_first = false,
                                       u32 next_old_size = 0, const NodeMutationContext *original = nullptr)
    {
        auto status = identity(epoch, original);
        if (!status) return fail(status);
        if (original && update.inode != original->key.inode)
            return fail(Status::fault("mounted mutation update inode is foreign"));
        status = CheckedOwnership ? owner_.prepare_ownership(update) : CheckedMode ? owner_.prepare_mode(update) :
            allocate_first && data ? owner_.prepare_first_data(
            {update.inode, update.size, update.ctime_seconds, update.ctime_nanoseconds}, first_offset, *data) :
            next_old_size && data ? owner_.prepare_next_data(
            {update.inode, next_old_size, update.size, update.ctime_seconds, update.ctime_nanoseconds},
            first_offset, *data) :
            data ? owner_.prepare_ordered_data(update, data_block, *data) :
            policy_ == Ext4MountedPolicy::maintenance_only ? owner_.template prepare_for<false>(update)
                                                          : owner_.prepare(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject_unprepared(status, epoch, original);
        }
        if (original)
        {
            const auto local = (update.inode - 1) % view_.inodes_per_group_;
            const auto table = view_.groups_[(update.inode - 1) / view_.inodes_per_group_].table + local / 4;
            bool found{};
            for (usize index = 0; index < owner_.participant_count_; ++index)
                if (owner_.blocks_[index] == table)
                {
                    found = true;
                    const auto inode = std::span<const u8>{owner_.sealed_[index]}.subspan((local % 4) * 256, 256);
                    if (auto checked = ext4_checked::inode_checksum(view_.checksum_seed(), update.inode, inode); !checked)
                        return fail(checked);
                    if (ext4_checked::le32(inode, 100) != original->key.generation)
                        return fail(Status::io_error("mounted prepared inode generation changed"));
                }
            if (!found) return fail(Status::fault("mounted prepared inode image is absent"));
        }
        // No public read is admitted between retiring this immutable view and
        // validating its replacement after checkpoint.
        if (status = identity(epoch, original); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch, original); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch, original); !status) return fail(status);
        // A reopened reader is insufficient: compare every changed home/control
        // block against both the sealed clean image and its canonical cache bytes.
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status) return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted checkpoint canonical image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (original)
        {
            auto changed = view_.inode(update.inode);
            if (!changed) return fail(changed.status());
            if (changed.value().generation != original->key.generation)
                return fail(Status::io_error("mounted renewed inode generation changed"));
        }
        auto metadata = this->metadata(update.inode);
        if (!metadata || metadata.value().mode != (expected_type | update.permissions) ||
            metadata.value().uid != update.uid || metadata.value().gid != update.gid)
            return fail(metadata ? Status::fault("mounted updated inode mismatch") : metadata.status());
        if (update.ordered_data && (metadata.value().size != update.size ||
            metadata.value().attributes.modification.seconds != update.mtime_seconds ||
            metadata.value().attributes.modification.nanoseconds != update.mtime_nanoseconds))
            return fail(Status::fault("mounted ordered-data size or mtime mismatch"));
        if (allocate_first && (metadata.value().blocks != 2 ||
            metadata.value().attributes.change.seconds != update.ctime_seconds ||
            metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted first data block or ctime mismatch"));
        if (next_old_size && (metadata.value().blocks != (next_old_size / 1024 + 1) * 2 ||
            metadata.value().attributes.change.seconds != update.ctime_seconds ||
            metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted next data block or ctime mismatch"));
        if ((CheckedOwnership || CheckedMode || policy_ == Ext4MountedPolicy::maintained_noatime) &&
            (metadata.value().attributes.change.seconds != update.ctime_seconds ||
             metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted updated ctime mismatch"));
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating)
                status = Status::fault("mounted admission epoch changed");
            else { ++report_.epoch; if (!original && !mutation_context_) report_.phase = Ext4MountPhase::readable; }
        }
        return status ? status : fail(status);
    }
    template <typename T> static void reset(T &object)
    { object.~T(); ::new (static_cast<void *>(&object)) T; }
    // Mount lock held. Root-domain failure quarantines the original Owner;
    // bookkeeping never follows a mutable token pointer to a decoy provider.
    [[nodiscard]] Status retention_integrity_locked()
    {
        if (!report_.epoch) return {};
        const auto status = root_retention_.binding(&nodes_[0], this, identity_.session_serial,
                                                   identity_.registration_generation);
        return status ? status : fail_locked(status);
    }
    [[nodiscard]] Status fail_locked(Status status)
    {
        // Owner failure only aborts its internal Cache/Session; no device I/O
        // or caller callbacks occur while the mount admission lock is held.
        if (startup_workspace_) owner_.startup_failed(status);
        root_retention_.stop_admission();
        static_cast<void>(owner_.fail(status));
        report_.phase = Ext4MountPhase::failed; report_.status = status; return status;
    }
    // Mount lock held. Neither an absent deferred pointer nor summary counters
    // erase actual Owner/Cache records. This private bounded check performs no I/O.
    [[nodiscard]] Status orphan_work_drained_locked()
    {
        if (pending_orphan_)
            return Status::busy("mounted orphan cleanup precedes drain");
        const auto status = owner_.mounted_teardown_preflight();
        return status || status.code() == StatusCode::busy ? status : fail_locked(status);
    }
    [[nodiscard]] Status drained_locked()
    {
        if (auto status = retention_integrity_locked(); !status) return status;
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase != Ext4MountPhase::draining)
            return Status::not_initialized("mounted provider is not draining");
        if (legacy_exposure_) return Status::busy("mounted provider has permanent borrowed exposure");
        if (auto status = root_retention_.empty(); !status) return status;
        if (report_.reading) return Status::busy("mounted work remains while draining");
        if (auto status = orphan_work_drained_locked(); !status) return status;
        if (auto status = lifetime_.audit_lease_records(this, identity_.session_serial); !status)
            return fail_locked(status);
        usize records{}, references{};
        for (const auto &node : nodes_)
        {
            usize held_count{};
            for (const auto &held : reference_holds_)
                if (held.node == &node)
                {
                    if (!held.lease || !held.lease->valid())
                        return fail_locked(Status::fault("mounted node lease record differs"));
                    if (auto status = lifetime_.validate_lease(*held.lease, node.lifetime_key_,
                                                              Ext4InodeHold::descriptor); !status)
                        return fail_locked(status);
                    ++held_count;
                }
            if (held_count != node.references_)
                return fail_locked(Status::fault("mounted node count differs from exact leases"));
            if (node.inode_)
            {
                const auto state = lifetime_.snapshot(node.lifetime_key_);
                if (!state || state.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] != held_count)
                    return fail_locked(Status::fault("mounted inode table differs from Node leases"));
                if (state.value().holds[static_cast<usize>(Ext4InodeHold::mapping)] ||
                    state.value().holds[static_cast<usize>(Ext4InodeHold::io)] ||
                    state.value().holds[static_cast<usize>(Ext4InodeHold::pin)])
                    return Status::busy("mounted inode has retained non-Node work");
            }
            references += held_count;
        }
        for (const auto &held : reference_holds_)
        {
            if (bool(held.node) != bool(held.lease))
                return fail_locked(Status::fault("mounted reference record differs"));
            if (held.node) ++records;
        }
        for (usize first = 0; first < reference_holds_.size(); ++first)
            if (reference_holds_[first].lease)
                for (usize second = first + 1; second < reference_holds_.size(); ++second)
                    if (reference_holds_[second].lease && Lifetime::same_lease(*reference_holds_[first].lease,
                                                                             *reference_holds_[second].lease))
                        return fail_locked(Status::fault("mounted Node lease attachment is duplicated"));
        if (records != references) return fail_locked(Status::fault("mounted reference names foreign Node"));
        if (references) return Status::busy("mounted node reference retained");
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight || buffers.transaction_active)
            return Status::busy("mounted buffers have retained work");
        return {};
    }
    // Mount lock held. The occupied original I/O role closes public admission
    // even if every mutable mount binding and diagnostic phase are erased.
    [[nodiscard]] Status mutation_admission_locked()
    {
        const auto active = lifetime_.node_mutation_active(this, identity_.session_serial);
        if (!active) return fail_locked(active.status());
        return active.value() ? Status::busy("mounted original mutation I/O remains admitted") : Status{};
    }
    [[nodiscard]] Status admission()
    {
        if (auto status = retention_integrity_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::failed) return Status::denied("mounted metadata is quarantined");
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::readable) return {};
        if (report_.phase == Ext4MountPhase::opening) return Status::busy("mounted metadata startup in flight");
        if (report_.phase == Ext4MountPhase::updating) return Status::busy("mounted metadata mutation in flight");
        if (report_.phase == Ext4MountPhase::draining) return Status::busy("mounted metadata is draining");
        return Status::not_initialized("mounted metadata unavailable");
    }
    [[nodiscard]] Status unsupported()
    { smp::ScopedSpinLock guard(lock_); auto status = admission(); return status ? Status::unsupported("metadata mount rejects ordinary mutation") : status; }
    [[nodiscard]] Status fail(Status status)
    {
        if (startup_workspace_) owner_.startup_failed(status);
        root_retention_.stop_admission();
        static_cast<void>(owner_.fail(status));
        smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::failed; report_.status = status; return status;
    }
    [[nodiscard]] Status identity(u64 epoch = 0, const NodeMutationContext *original = nullptr) const
    {
        if (epoch)
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || owner_.publication_domain_ != epoch ||
                report_.phase != Ext4MountPhase::updating || report_.reading)
                return Status::fault("mounted publication epoch changed");
            if (auto status = mutation_integrity_locked(original); !status) return status;
        }
        const auto now = owner_.buffers();
        const auto session = owner_.session_report();
        if (!session || session.value().state != block::DeviceState::owned ||
            session.value().identity != identity_.device_identity ||
            session.value().registration_generation != identity_.registration_generation ||
            session.value().session_serial != identity_.session_serial)
            return Status::fault("mounted session is not authoritative");
        if (now.state != MetadataOwnerState::ready || now.device_identity != identity_.device_identity ||
            now.registration_generation != identity_.registration_generation || now.session_serial != identity_.session_serial ||
            now.first_sector != identity_.first_sector || now.sectors != identity_.sectors || now.blocks != identity_.blocks)
            return Status::fault("mounted session authority changed");
        return {};
    }
    [[nodiscard]] Status inspect(bool renewed)
    {
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
            policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime,
            policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime);
        inspection_.require_one_kib();
        auto &device = owner_.startup_domain_active() ? static_cast<driver::BlockDevice &>(*owner_.startup_reader_) : owner_.bound_device();
        if (auto status = inspection_.prepare(device, owner_.bounds_); !status) return status;
        const auto &value = *inspection_.binding();
        if (value.needs_recovery != (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime) ||
            !value.empty_log || value.layout.block_size != 1024)
            return Status::unsupported("metadata mount requires selected empty 1 KiB journal state");
        if (renewed)
        {
            const auto &a = binding_.layout; const auto &b = value.layout;
            if (a.first_sector != b.first_sector || a.filesystem_blocks != b.filesystem_blocks ||
                a.journal_first_block != b.journal_first_block || a.journal_blocks != b.journal_blocks ||
                a.block_size != b.block_size || binding_.uuid != value.uuid ||
                binding_.journal_inode != value.journal_inode || binding_.metadata_count != value.metadata_count ||
                value.journal_sequence != owner_.report().completion.journal_sequence)
                return Status::fault("mounted binding changed at renewal");
            for (usize index = 0; index < value.metadata_count; ++index)
                if (value.metadata[index].first != binding_.metadata[index].first ||
                    value.metadata[index].count != binding_.metadata[index].count)
                    return Status::fault("mounted metadata domain changed at renewal");
        }
        binding_ = value; return identity();
    }
    [[nodiscard]] Status recover_and_mark_dirty()
    {
        auto &startup_device = owner_.startup_domain_active() ?
            static_cast<driver::BlockDevice &>(*owner_.startup_ordinary_reader_) : owner_.bound_device();
        if (owner_.startup_legacy_selected_)
        {
            if (auto status = owner_.startup_domain_guard(); !status) return status;
            const auto actions = owner_.startup_legacy_actions_;
            const auto replay = actions == 1 ? Ext4ReplayPolicy::mounted_inode :
                actions < 4 ? Ext4ReplayPolicy::directory_rename :
                actions == 4 ? Ext4ReplayPolicy::bounded_data_allocation :
                actions <= 6 ? Ext4ReplayPolicy::directory_create : Ext4ReplayPolicy::bounded_directory_create;
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                replay == Ext4ReplayPolicy::mounted_inode, true, replay);
            // The private original selection and complete strict proof precede
            // every recovery WRITE/FLUSH. Never retry another policy on failure.
            return inspection_.run_legacy_startup(startup_device, owner_.bounds_,
                owner_.startup_workspace_->legacy_head_seal_,
                owner_.startup_workspace_->legacy_descriptor_seal_, actions);
        }
        // Recognize the union of exact four-action allocation proofs before the legacy
        // structural gate, including an all-after checkpoint. A recognized
        // allocation proof failure can never fall through to a weaker policy.
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
            false, true, Ext4ReplayPolicy::bounded_data_allocation);
        Ext4ReplayPolicy replay = Ext4ReplayPolicy::bounded_data_allocation;
        auto status = inspection_.run(startup_device, owner_.bounds_);
        if (!status)
        {
            const auto &failed = inspection_.report();
            const bool other_shape = status.code() == StatusCode::unsupported &&
                status.message() == "ext4 first data replay transaction shape";
            const bool create_split = status.code() == StatusCode::io_error &&
                (status.message() == "ext4 recovery inode bitmap checksum" ||
                 status.message() == "ext4 recovery block bitmap checksum");
            if ((!other_shape && !create_split) || failed.write_attempts || failed.flush_attempts)
                return status;
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(true, true, Ext4ReplayPolicy::mounted_inode);
            replay = Ext4ReplayPolicy::mounted_inode;
            status = inspection_.run(startup_device, owner_.bounds_);
        }
        if (!status)
        {
            const auto &failed = inspection_.report();
            const bool target_denied = status.code() == StatusCode::denied ||
                (status.code() == StatusCode::unsupported &&
                 status.message() == "checked metadata replay transaction shape");
            const bool provisional_bitmap = status.code() == StatusCode::io_error &&
                (status.message() == "ext4 recovery inode bitmap checksum" ||
                 status.message() == "ext4 recovery block bitmap checksum");
            if ((!target_denied && !provisional_bitmap) || failed.write_attempts || failed.flush_attempts)
                return status;
            // A different action shape refused before I/O. A create can leave
            // its bitmap and descriptor home checksums split at a crash cut;
            // only the exact create replay proof may admit that state.
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                false, true, Ext4ReplayPolicy::bounded_directory_create);
            replay = Ext4ReplayPolicy::bounded_directory_create;
            status = inspection_.run(startup_device, owner_.bounds_);
            if (!status)
            {
                const auto &mkdir_failed = inspection_.report();
                if (status.code() != StatusCode::unsupported ||
                    status.message() != "ext4 create replay transaction shape" ||
                    mkdir_failed.write_attempts || mkdir_failed.flush_attempts)
                    return status;
                inspection_.~Ext4RecoveryCompletion<8, 2>();
                ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                    false, true, Ext4ReplayPolicy::directory_create);
                replay = Ext4ReplayPolicy::directory_create;
                status = inspection_.run(startup_device, owner_.bounds_);
            }
            if (!status)
            {
                const auto &create_failed = inspection_.report();
                if (!target_denied || status.code() != StatusCode::unsupported ||
                    status.message() != "ext4 create replay transaction shape" ||
                    create_failed.write_attempts || create_failed.flush_attempts)
                    return status;
                inspection_.~Ext4RecoveryCompletion<8, 2>();
                ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                    false, true, Ext4ReplayPolicy::directory_rename);
                replay = Ext4ReplayPolicy::directory_rename;
                status = inspection_.run(startup_device, owner_.bounds_);
                if (!status) return status;
            }
        }
        if ((ext4_create_replay_policy(replay) || replay == Ext4ReplayPolicy::directory_rename) &&
            inspection_.report().empty_log)
            return Status::fault("mounted directory recovery has no committed transaction");
        if (!inspection_.report().empty_log &&
            (replay == Ext4ReplayPolicy::directory_rename ?
                (inspection_.report().validated_actions != 2 && inspection_.report().validated_actions != 3) :
             replay == Ext4ReplayPolicy::bounded_directory_create ?
                (inspection_.report().validated_actions != 7 && inspection_.report().validated_actions != 8) :
             replay == Ext4ReplayPolicy::directory_create ?
                (inspection_.report().validated_actions != 5 && inspection_.report().validated_actions != 6) :
             ext4_data_allocation_policy(replay) ? inspection_.report().validated_actions != 4 :
                inspection_.report().validated_actions != 1))
            return Status::fault("mounted recovery action count");
        if (!inspection_.report().already_clean) return {};
        auto super = inspection_.filesystem_;
        Ext4RecoveryCompletion<8, 2>::le32(super, 96, ext4_checked::le32(super, 96) | u32{4});
        Ext4RecoveryCompletion<8, 2>::le32(super, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        auto &device = startup_device;
        if (auto status = device.write_blocks(2, std::as_bytes(std::span{super})); !status) return status;
        return device.flush();
    }
    template <typename Operation> auto read_operation(Operation operation, bool transport = false,
                                                      const Ext4InodeLifetimeKey *key = nullptr,
                                                      const MountedNode *node = nullptr,
                                                      bool persistent = false) -> decltype(operation())
    {
        using ResultType = decltype(operation());
        std::optional<Lease> in_flight;
        Ext4InodeLifetimeKey original_key{};
        Status acquire_status{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return ResultType{status};
            if (report_.reading) return ResultType{Status::busy("mounted reader already admitted")};
            if (node)
            {
                if (auto status = authentic_node_locked(*node); !status) return ResultType{status};
                if (persistent)
                    if (auto status = persistent_node_locked(*node); !status) return ResultType{status};
                original_key = node->lifetime_key_; key = &original_key;
            }
            if (key)
            {
                auto held = retain_existing(*key, Ext4InodeHold::io);
                if (!held) acquire_status = held.status();
                else in_flight.emplace(std::move(held).value());
            }
            if (acquire_status) report_.reading = true;
        }
        if (!acquire_status)
            return ResultType{acquire_status.code() == StatusCode::no_memory ||
                              acquire_status.code() == StatusCode::overflow ||
                              acquire_status.code() == StatusCode::busy ?
                                  acquire_status : fail(acquire_status)};
        auto identity_status = identity();
        const bool authority_failed = !identity_status;
        bool checked_fatal = false;
        if (identity_status && key)
        {
            auto checked = view_.inode(key->inode);
            if (!checked)
            {
                identity_status = checked.status();
                checked_fatal = checked.status().code() != StatusCode::no_memory &&
                                checked.status().code() != StatusCode::overflow;
            }
            else if (checked.value().generation != key->generation)
            {
                identity_status = Status::io_error("mounted inode generation changed during read");
                checked_fatal = true;
            }
            if (identity_status && node)
            {
                identity_status = physical_node_generation(*key);
                checked_fatal = !identity_status;
            }
        }
        if (identity_status && node)
        {
            smp::ScopedSpinLock guard(lock_);
            identity_status = authentic_node_locked(*node);
            if (identity_status && node->lifetime_key_ != original_key)
                identity_status = Status::fault("mounted original zero-write Node changed");
            if (identity_status) identity_status = lifetime_.validate_lease(*in_flight, original_key, Ext4InodeHold::io);
            checked_fatal = !identity_status;
        }
        auto result = identity_status ? operation() : ResultType{identity_status};
        Status status = [&]() { if constexpr (std::is_same_v<ResultType, Status>) return result; else return result.status(); }();
        if (persistent && status)
        {
            auto checked = identity();
            if (checked)
            {
                smp::ScopedSpinLock guard(lock_);
                checked = persistent_node_locked(*node);
                if (checked && node->lifetime_key_ != original_key)
                    checked = Status::fault("persistent sync original node changed during I/O");
                if (checked) checked = lifetime_.validate_lease(*in_flight, original_key, Ext4InodeHold::io);
            }
            if (!checked) { status = checked; checked_fatal = true; }
        }
        const Status released = in_flight ? lifetime_.release(*in_flight) : Status{};
        // Reader I/O errors retire the view. Benign lookup/capacity errors do not.
        const bool fatal = authority_failed || checked_fatal || !released || !view_.valid() || (!status && (transport || (status.code() != StatusCode::not_found &&
            status.code() != StatusCode::unsupported && status.code() != StatusCode::overflow &&
            status.code() != StatusCode::not_directory && status.code() != StatusCode::invalid_argument &&
            status.code() != StatusCode::is_directory && status.code() != StatusCode::no_memory)));
        if (fatal)
        {
            const auto failure = fail(!released ? released :
                                      status ? Status::fault("mounted view retired") : status);
            { smp::ScopedSpinLock guard(lock_); report_.reading = false; }
            return ResultType{failure};
        }
        { smp::ScopedSpinLock guard(lock_); report_.reading = false; }
        const auto cleanup = drain_pending_orphan();
        if (status && !cleanup) return ResultType{cleanup};
        return result;
    }
    // Required before a future worker retires or clears a Node identity. Raw
    // legacy exposure has no revocable token and therefore permanently bars
    // that slot's retirement/reuse for this provider lifetime. No freeing here.
    [[nodiscard]] Status validate_node_retirement(const typename Lifetime::Ticket &ticket) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.reading || report_.phase != Ext4MountPhase::updating)
            return Status::busy("mounted node retirement requires exclusive update");
        const auto key = ticket.key();
        if (key.mount != this || key.session != identity_.session_serial)
            return Status::bad_descriptor("mounted node retirement identity is foreign");
        if (key.inode < 11)
            return Status::unsupported("mounted reserved inode retirement is forbidden");
        if (auto status = lifetime_.validate_dispatched(ticket, key); !status) return status;
        for (const auto &node : nodes_)
            if (node.inode_ == key.inode && node.lifetime_key_ == key)
                return node.references_ || node.borrowed_identity_exposed_ ?
                    Status::busy("mounted node identity remains exposed") : Status{};
        return Status::not_found("mounted node retirement identity is absent");
    }

    // Mount lock held. Reclaims the slot of an identity nothing can reach: no
    // references or leases, never handed out raw, and not the root, the
    // pending orphan or the node under mutation. Its lifetime record goes with
    // it; a later lookup observes the inode afresh from the checked image.
    [[nodiscard]] MountedNode *evict_locked()
    {
        for (usize index = 1; index < nodes_.size(); ++index)
        {
            auto &node = nodes_[index];
            if (!node.inode_ || node.references_ || node.borrowed_identity_exposed_ ||
                &node == pending_orphan_ || &node == mutation_node_)
                continue;
            bool held{};
            for (const auto &hold : reference_holds_) held = held || hold.node == &node;
            if (held || !lifetime_.forget(node.lifetime_key_)) continue;
            node.inode_ = 0; node.lifetime_key_ = {}; node.name_ = {};
            node.rename_names_ = {}; node.rename_count_ = 0;
            return &node;
        }
        return nullptr;
    }
    [[nodiscard]] Result<Node *> node(u32 inode, std::string_view name)
    {
        auto checked = view_.inode(inode);
        if (!checked) return checked.status();
        const Ext4InodeLifetimeKey key{this, identity_.session_serial, inode,
                                       checked.value().generation};
        smp::ScopedSpinLock guard(lock_);
        for (auto &node : nodes_) if (node.inode_ == inode)
            return node.lifetime_key_ == key ? Result<Node *>{static_cast<Node *>(&node)} :
                Result<Node *>{Status::io_error("mounted inode generation changed under a live node")};
        MountedNode *slot{};
        for (auto &node : nodes_) if (!node.inode_ && !node.borrowed_identity_exposed_) { slot = &node; break; }
        if (!slot) slot = evict_locked();
        if (!slot) return Status::no_memory("mounted metadata node capacity");
        if (auto status = slot->name_.assign(name); !status) return status;
        if (auto status = lifetime_.observe(key); !status)
        { slot->name_ = {}; return status; }
        slot->mount_ = this; slot->inode_ = inode; slot->lifetime_key_ = key;
        return static_cast<Node *>(slot);
    }
    const Ext4MountedPolicy policy_;
    Ext4MountedReclaimWorkspace *reclaim_workspace_{};
    Ext4MountedStartupWorkspace *startup_workspace_{};
    CommonStorage *common_storage_{};
    MountedNode *pending_orphan_{};
    u32 orphan_seconds_{}, orphan_nanoseconds_{};
    Owner owner_{};
    typename Owner::StartupReadView startup_reader_{owner_};
    typename Owner::StartupOrdinaryView startup_ordinary_reader_{owner_};
    Reader reader_{this};
    View view_{};
    Ext4RecoveryCompletion<8, 2> inspection_{};
    Ext4RecoveryInfo binding_{};
    MetadataOwnerReport identity_{};
    std::array<MountedNode, 128> nodes_{};
    Lifetime lifetime_{};
    std::array<ReferenceHold, 512> reference_holds_{};
    MountedNode *mutation_node_{};
    Ext4InodeLifetimeKey mutation_key_{};
    Lease *mutation_lease_{};
    const NodeMutationContext *mutation_context_{};
    MountedRootRetentionDomain root_retention_{};
    bool legacy_exposure_{};
    Ext4MountReport report_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::fs
