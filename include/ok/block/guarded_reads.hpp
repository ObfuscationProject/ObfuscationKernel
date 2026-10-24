#pragma once

#include "ok/block/sessions.hpp"
#include <concepts>

namespace ok::block
{
template <typename Guard, typename Session>
concept ReadDispatchGuard = requires(Guard &guard, Session &session, u64 first, usize bytes, Status status)
{
    { guard.begin_read(session, first, bytes) } -> std::same_as<Status>;
    { guard.finish_read(session, status) } -> std::same_as<Status>;
    { session.read_blocks(first, std::span<std::byte>{}) } -> std::same_as<Status>;
    { session.abort(status) } -> std::same_as<Status>;
};

// All storage is borrowed only for this serialized call. Hooks cannot perform
// transport I/O or reenter it. Begin refusal leaves the Session untouched.
template <typename Session, typename Guard> requires ReadDispatchGuard<Guard, Session>
[[nodiscard]] Status guarded_read_blocks(Session &session, Guard &guard,
                                         u64 first, std::span<std::byte> bytes)
{
    if (auto status = guard.begin_read(session, first, bytes.size()); !status) return status;
    auto status = [&]() -> Status {
        if constexpr (requires { session.read_blocks_guarded(first, bytes, guard); })
            return session.read_blocks_guarded(first, bytes, guard);
        else return session.read_blocks(first, bytes);
    }();
    const auto accounted = guard.finish_read(session, status);
    if (status && !accounted) status = accounted;
    if (!status) static_cast<void>(session.abort(status));
    return status;
}
} // namespace ok::block
