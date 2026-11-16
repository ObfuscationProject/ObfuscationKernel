#pragma once

#include "ok/arch/execution.hpp"
#include "ok/sched/scheduler.hpp"
#include <bit>

namespace ok { class Kernel; }
namespace ok::user::native
{
// Copied scalar request. No process or user-buffer pointer is retained across
// blocking. Siginfo output is RV32 waitid. The selector is the raw wait4 pid
// (or waitid id); a non-zero `group` selects children by process group, which
// for selector 0 or P_PGID 0 is the caller's group at call time. Appended
// fields keep older brace-initializers valid.
struct ChildWaitRequest
{
    i32 selector{};
    uptr output{};
    bool nohang{}, siginfo{};
    bool big_endian{std::endian::native == std::endian::big};
    u32 group{};
    bool exited{true}, stopped{}, continued{}, nowait{};
    u8 idtype{};        // waitid P_ALL 0, P_PID 1, P_PGID 2.
    u32 raw_options{};  // Exact options word, revalidated by the restart binding.
    [[nodiscard]] constexpr bool operator==(const ChildWaitRequest &) const = default;
};
// The scheduler wait key of a blocked request: the group key, any child (0),
// or the child pid. Shared by the wait service and its restart binding.
[[nodiscard]] constexpr u64 child_wait_key(const ChildWaitRequest &request)
{
    if (request.group) return sched::SchedulerState::child_group_wait_key(request.group);
    return request.selector == -1 ? 0 : static_cast<u32>(request.selector);
}
// Which report a ready child carries.
enum class ChildWaitReport : u8 { exited, stopped, continued };
i64 begin_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete);
} // namespace ok::user::native
