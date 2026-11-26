#pragma once
// The delivery point of a pending exit (spec 4.8) for hosted runtime models:
// a kill or fatal decision only sets the group's pending exit, and the
// group's next picked thread applies it. Models with no step call this where
// that pick would come; each process with a pending exit exits as its picked
// thread would.
#include <array>

namespace ok::user::native
{
inline usize deliver_pending_exits(Kernel &kernel)
{
    std::array<sched::ProcessId, sched::max_processes> pending{};
    usize count = 0;
    for (const auto &process : kernel.scheduler().processes())
        if (kernel.scheduler().exit_pending(process.pid())) pending[count++] = process.pid();
    for (usize i = 0; i < count; ++i) static_cast<void>(deliver_pending_exit(kernel, pending[i]));
    return count;
}
} // namespace ok::user::native
