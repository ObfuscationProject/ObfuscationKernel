#pragma once

#include "ok/core/kernel.hpp"

namespace ok::user::native
{
// Both syscall personalities enter here with a copied, typed request. This
// helper owns boot-CPU deadline admission; completion remains in runtime::step.
inline Status begin_sleep(Kernel &kernel, sched::ProcessId pid, u64 now,
                          posix::ClockTime request, bool absolute, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return Status::invalid_argument("sleep owner is not active");
    auto &thread = *current;
    auto armed = thread.native_deadline.arm(now, request.seconds, request.nanoseconds, absolute);
    if (!armed)
        return armed.status();
    if (thread.native_deadline.complete(now, armed.value()))
        return Status::success();
    auto status = kernel.scheduler().block_current(sched::ThreadWaitReason::deadline, armed.value());
    if (!status)
    {
        thread.native_deadline.cancel();
        return status;
    }
    // Successful sleep leaves remainder untouched. The pending syscall frame
    // retains the original remainder address for qualified Linux interruption.
    complete = false;
    return Status::success();
}
} // namespace ok::user::native
