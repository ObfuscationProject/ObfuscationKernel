#pragma once

#include "ok/core/kernel.hpp"

namespace ok::user::native
{
// Both syscall personalities enter here with a copied, typed request. The
// wait's deadline goes into the scheduler's deadline heap (spec 4.6); its
// expiry completes the wait as timed out and the owner's dispatch returns 0.
inline Status begin_sleep(Kernel &kernel, sched::ProcessId pid, u64 now,
                          posix::ClockTime request, bool absolute, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return Status::invalid_argument("sleep owner is not active");
    auto &thread = *current;
    // The record of a sleep whose wait was cancelled without reaching its
    // owner's resume is stale (the owner settles its record, not the canceller).
    if (thread.native_deadline.armed() && !thread.wait_registration.active()) thread.native_deadline.cancel();
    auto armed = thread.native_deadline.arm(now, request.seconds, request.nanoseconds, absolute);
    if (!armed)
        return armed.status();
    if (thread.native_deadline.complete(now, armed.value()))
        return Status::success();
    auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, armed.value());
    if (!ticket)
    {
        thread.native_deadline.cancel();
        return ticket.status();
    }
    static_cast<void>(kernel.scheduler().arm_wait_deadline(ticket.value(), thread.native_deadline.deadline()));
    // Successful sleep leaves remainder untouched. The pending syscall frame
    // retains the original remainder address for qualified Linux interruption.
    complete = false;
    return Status::success();
}
} // namespace ok::user::native
