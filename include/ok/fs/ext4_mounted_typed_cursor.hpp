#pragma once

#include "ok/fs/ext4_mounted_typed_budget.hpp"
#include "ok/fs/ext4_direct_read_scope.hpp"
#include "ok/fs/ext4_mounted_common_opening_budget.hpp"
#include <concepts>

namespace ok::fs {
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
} // namespace ok::fs
