#pragma once

#include "ok/sched/console_input.hpp"

namespace ok::tests
{
// Lease algorithm only; no native trap, terminal frontend or password UI claim.
// Input bytes live in the console terminal's line discipline, not the lease.
inline Status check_console_input_lease_model()
{
    sched::ConsoleInputLease input;
    if (input.admit() || input.authorized(2, 0) || !input.bind_manager(1) ||
        input.bind_manager(1) || input.claim(2, 3) || input.claim(1, 1))
        return Status::fault("console lease default-closed authority model failed");
    auto first = input.claim(1, 2);
    if (!first || !input.admit() || input.authorized(3, 0, first.value()) ||
        !input.authorized(2, first.value(), first.value()) || input.inherit(first.value()) != first.value())
        return Status::fault("console lease exclusive issuance model failed");
    if (input.flush(3, first.value(), first.value()) || !input.flush(2, first.value(), first.value()) ||
        !input.flush(1, 0, first.value()))
        return Status::fault("console lease flush authority model failed");
    if (input.revoke(3, first.value()) || !input.revoke(1, first.value()) || input.admit() ||
        input.inherit(first.value()))
        return Status::fault("console lease revocation model failed");
    auto next = input.claim(1, 4);
    if (!next || next.value() == first.value() || input.revoke(1, first.value()) ||
        input.authorized(2, first.value(), first.value()) || !input.retire(4) || input.admit() || input.active())
        return Status::fault("console lease stale generation or leader retirement model failed");
    auto last = input.claim(1, 5);
    if (!last || !input.retire(1) || input.claim(1, 6) || input.admit())
        return Status::fault("console lease manager retirement model failed");
    return {};
}
} // namespace ok::tests
