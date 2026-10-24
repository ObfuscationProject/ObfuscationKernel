#pragma once

#include "ok/arch/execution.hpp"
#include "ok/sched/scheduler.hpp"
#include <bit>

namespace ok { class Kernel; }
namespace ok::user::native
{
// Copied scalar request. No process or user-buffer pointer is retained across
// blocking. Siginfo output is the existing RV32 WEXITED/P_ALL/P_PID subset.
struct ChildWaitRequest
{
    i32 selector{};
    uptr output{};
    bool nohang{}, siginfo{};
    bool big_endian{std::endian::native == std::endian::big};
    [[nodiscard]] constexpr bool operator==(const ChildWaitRequest &) const = default;
};
i64 begin_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete);
} // namespace ok::user::native
