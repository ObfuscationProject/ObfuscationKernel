#pragma once

#include "ok/block/guarded_reads.hpp"
#include "ok/fs/ext4_reclaim_history.hpp"

namespace ok::fs
{
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
} // namespace ok::fs
