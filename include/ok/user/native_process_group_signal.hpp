#pragma once

#include "ok/sched/scheduler.hpp"

namespace ok::user
{
// Copied before recipient callbacks; scheduler compaction cannot revoke it.
struct ProcessSignalSender
{
    sched::ProcessId pid{};
    Credentials credentials{};
    sched::ProcessId group{};
};

[[nodiscard]] inline ProcessSignalSender process_signal_sender(const sched::ProcessControlBlock &sender)
{
    return {sender.pid(), sender.credentials(), sender.process_group_id()};
}

// The boot CPU serializes selection and callbacks. Snapshot only PIDs, never
// borrowed PCB/TCB references. Callbacks re-resolve recipients and own delivery.
template <u32 MaximumSignal, typename Deliver>
[[nodiscard]] i64 send_process_group_signal(sched::SchedulerState &scheduler,
                                            ProcessSignalSender sender,
                                            u64 raw_selector, u64 raw_signal,
                                            Deliver &&deliver)
{
    const auto selector = static_cast<i32>(raw_selector);
    const auto signal = static_cast<i32>(raw_signal);
    if (signal < 0 || signal > static_cast<i32>(MaximumSignal)) return -22;
    if (selector > 0) return -22;
    if (selector == -1) return -95;
    // Linux kill_something_info refuses INT_MIN before negating pid_t.
    if (selector == (-2147483647 - 1)) return -3;
    if (signal != 0 && signal != 9 && signal != 15) return -95;
    const auto group = selector == 0 ? sender.group
                                    : static_cast<sched::ProcessId>(-static_cast<i64>(selector));
    if (!group) return -3;
    std::array<sched::ProcessId, sched::max_processes> targets{};
    usize count = 0;
    for (const auto &process : scheduler.processes())
        if (process.process_group_id() == group)
        {
            if (count == targets.size()) return -12;
            targets[count++] = process.pid();
        }
    i64 result = -3;
    bool accepted = false;
    for (usize index = 0; index < count; ++index)
    {
        const auto delivered = deliver(sender, targets[index], signal);
        accepted |= delivered == 0;
        if (!accepted) result = delivered;
    }
    return accepted ? 0 : result;
}
} // namespace ok::user
