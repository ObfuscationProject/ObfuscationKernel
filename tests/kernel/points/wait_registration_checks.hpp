#pragma once

#include "ok/sched/wait_registration.hpp"

namespace ok::sched
{
struct WaitRegistrationTestAccess
{
    static WaitRegistration near_exhaustion()
    {
        WaitRegistration wait;
        wait.ticket_.generation = ~u64{0} - 1;
        return wait;
    }
};
} // namespace ok::sched

namespace ok::tests
{
// These checks exercise ownership metadata even when booted in a real guest;
// native blocking/user execution remains a separate runtime acceptance gate.
inline Status check_wait_registration()
{
    using sched::WaitCompletion;
    using sched::WaitTicket;
    sched::WaitRegistration wait;
    if (wait.active() || wait.ticket() != WaitTicket{} || wait.completion() != WaitCompletion::none ||
        wait.cancel() || wait.pending({}) || wait.complete({}, WaitCompletion::ready))
        return Status::fault("empty wait registration accepted completion");
    if (wait.arm(0, 1).status().code() != StatusCode::invalid_argument ||
        wait.arm(1, 0).status().code() != StatusCode::invalid_argument || wait.ticket() != WaitTicket{})
        return Status::fault("invalid wait identities changed registration");
    const auto first = wait.arm(7, 11);
    if (!first || first.value() != WaitTicket{7, 11, 1} || !wait.pending(first.value()) ||
        wait.completion() != WaitCompletion::none)
        return Status::fault("valid wait registration was not published");
    if (wait.arm(7, 11).status().code() != StatusCode::busy || wait.ticket() != first.value() ||
        wait.complete({8, 11, 1}, WaitCompletion::ready) ||
        wait.complete({7, 12, 1}, WaitCompletion::ready) ||
        wait.complete({7, 11, 2}, WaitCompletion::ready) ||
        wait.complete(first.value(), WaitCompletion::none) ||
        wait.complete(first.value(), static_cast<WaitCompletion>(255)) || !wait.pending(first.value()))
        return Status::fault("foreign or invalid completion replaced active wait");
    if (!wait.complete(first.value(), WaitCompletion::ready) || wait.active() ||
        wait.pending(first.value()) || wait.completion() != WaitCompletion::ready ||
        wait.complete(first.value(), WaitCompletion::timeout) || wait.cancel() ||
        wait.completion() != WaitCompletion::ready)
        return Status::fault("wait completion was not singular");
    const auto second = wait.arm(7, 11);
    if (!second || second.value() != WaitTicket{7, 11, 2} ||
        wait.complete(first.value(), WaitCompletion::timeout) || !wait.pending(second.value()) ||
        !wait.cancel() || wait.cancel() || wait.completion() != WaitCompletion::cancelled)
        return Status::fault("cancellation reused a stale generation");
    const auto third = wait.arm(8, 12);
    if (!third || third.value() != WaitTicket{8, 12, 3} || wait.completion() != WaitCompletion::none ||
        wait.complete(second.value(), WaitCompletion::ready) ||
        !wait.complete(third.value(), WaitCompletion::timeout) || wait.completion() != WaitCompletion::timeout)
        return Status::fault("replacement wait lost identity or deadline outcome");
    const auto interrupted = wait.arm(8, 12);
    if (!interrupted || !wait.complete(interrupted.value(), WaitCompletion::interrupted) || wait.active() ||
        wait.completion() != WaitCompletion::interrupted || wait.complete(interrupted.value(), WaitCompletion::ready) ||
        wait.complete(interrupted.value(), WaitCompletion::timeout) || wait.cancel())
        return Status::fault("signal interruption lost singular wait completion");
    auto exhausted = sched::WaitRegistrationTestAccess::near_exhaustion();
    const auto last = exhausted.arm(~u64{0}, ~u64{0});
    if (!last || last.value() != WaitTicket{~u64{0}, ~u64{0}, ~u64{0}} ||
        exhausted.arm(1, 1).status().code() != StatusCode::busy || !exhausted.cancel() ||
        exhausted.arm(1, 1).status().code() != StatusCode::overflow || exhausted.active() ||
        exhausted.ticket() != last.value() || exhausted.completion() != WaitCompletion::cancelled ||
        exhausted.complete(last.value(), WaitCompletion::ready))
        return Status::fault("wait generation wrapped or exhaustion changed completion");
    return Status::success();
}
} // namespace ok::tests
