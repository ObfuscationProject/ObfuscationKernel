#pragma once
#include "ok/sched/deadline.hpp"

namespace ok::tests
{
inline Status check_deadline_wait()
{
    sched::DeadlineWait wait;
    auto first = wait.arm(100, 0, 50, false);
    if (!first || wait.deadline() != 150 || wait.complete(149, first.value()) ||
        wait.complete(150, first.value() + 1) || !wait.complete(150, first.value()) ||
        wait.complete(200, first.value()) || wait.armed())
        return Status::fault("deadline must complete once at its absolute target");
    auto canceled = wait.arm(200, 0, 50, false);
    if (!canceled)
        return canceled.status();
    wait.cancel();
    auto replacement = wait.arm(0, 0, 300, true);
    if (!replacement || wait.complete(400, canceled.value()) || !wait.armed() ||
        !wait.complete(300, replacement.value()))
        return Status::fault("canceled deadline token was reused");
    auto past = wait.arm(500, 0, 1, true);
    if (!past || !wait.complete(500, past.value()))
        return Status::fault("past absolute deadline did not complete");
    if (wait.arm(0, -1, 0, false) || wait.arm(0, 0, -1, false) ||
        wait.arm(0, 0, 1000000000, false) || wait.arm(~u64{0}, 0, 1, false) ||
        wait.arm(0, static_cast<i64>((~u64{0}) >> 1), 0, false) || wait.armed())
        return Status::fault("invalid deadline changed wait state");
    auto active = wait.arm(0, 1, 0, true);
    if (!active || wait.arm(0, 0, 1, false) || wait.deadline() != 1000000000)
        return Status::fault("active deadline was replaced");
    wait.cancel();
    if (wait.complete(2000000000, active.value()))
        return Status::fault("canceled deadline completed");
    return Status::success();
}
} // namespace ok::tests
