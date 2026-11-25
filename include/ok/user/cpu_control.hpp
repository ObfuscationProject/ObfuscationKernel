#pragma once

#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

namespace ok
{
class Kernel;
}

namespace ok::user::native
{
// The write route of /sys/devices/system/cpu/cpuN/online (docs/modules/sysfs.md,
// src/user/runtime/hotplug.cpp). write, writev and pwrite64 of a description
// whose control node is cpu_online come here before any node path; the call
// either completes (a no-op, a refusal, a parse error) or files a request for
// CPU 0's hotplug service and waits uninterruptibly (complete=false). The
// step's resume chain hands the woken writer its stored result when it
// dispatches the call again, so the re-dispatch never reaches this route and
// never reads the user buffer again.
i64 cpu_control_write(Kernel &kernel, sched::ProcessId pid, u16 cpu, uptr buffer, usize count, bool &complete);
i64 cpu_control_writev(Kernel &kernel, sched::ProcessId pid, u16 cpu, uptr vectors, usize vector_count,
                       bool &complete);
} // namespace ok::user::native
