#pragma once

// quiesce_group (spec 4.8): a group transition's claimer freezes its thread
// group and waits until no other thread of the group runs on a CPU. While it
// waits it holds no spin lock and, when it holds the big kernel lock, it
// releases it (rule W0): a sibling that trapped out of user mode takes the
// lock, finds its group frozen and goes to put_prev without dispatching.
// The host supplies the lock, interrupt service and clock (static dispatch:
// the runtime's on a kernel CPU, a host-thread model's in tests).

#include "ok/sched/scheduler.hpp"
#include "ok/smp/bringup.hpp"

#include <concepts>
#include <optional>

namespace ok::sched
{

template <typename Host>
concept QuiesceHost = requires(Host &host) {
    { host.kernel_lock_held() } -> std::same_as<bool>;
    host.release_kernel_lock();
    host.retake_kernel_lock();
    host.serve();
    { host.now_ns() } -> std::same_as<u64>;
};

enum class QuiesceResult : u8
{
    quiesced, // This thread holds the claim; no other thread of the group runs.
    lost,     // Another transition holds the group: return to the boundary.
    departed, // The process is not live.
};

inline constexpr u64 quiesce_report_ns = 2'000'000'000;

// Waits until no thread of `pid` other than `claimer` is on a CPU. Each
// quiesce_report_ns without progress counts one quiesce_timeouts and kicks
// again; the wait continues, since going on while a sibling may run would
// be unsafe. Returns with the kernel lock held again if it was held.
template <QuiesceHost Host> void wait_group_quiesced(SchedulerState &scheduler, ProcessId pid, ThreadId claimer,
                                                    Host &host)
{
    if (scheduler.group_quiesced(pid, claimer)) return;
    static_cast<void>(smp::check_class_wait("quiesce_group under a spin lock"));
    if (const auto self = smp::current_cpu(); self < smp::max_cpus)
        smp::detail::owner_add(smp::evidence_cpus[self].group_waits);
    const bool held = host.kernel_lock_held();
    if (held) host.release_kernel_lock();
    {
        const smp::SpinningScope spinning;
        auto mark = host.now_ns();
        while (!scheduler.group_quiesced(pid, claimer))
        {
            host.serve();
            arch::ArchOperations{}.spin_pause();
            if (const auto now = host.now_ns(); now - mark >= quiesce_report_ns)
            {
                smp::count_check(smp::KernelCheck::quiesce_timeouts, "quiesce_group waited 2 s");
                const auto self = smp::current_cpu();
                const auto cpus = scheduler.group_cpus(pid, claimer);
                for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
                    if (cpu != self && (cpus & (1u << cpu)) != 0) smp::send_ipi(cpu, smp::ipi_kick);
                mark = now;
            }
        }
    }
    if (held) host.retake_kernel_lock();
}

// Claims `kind` for `claimer` and, once claimed, kicks every CPU that runs
// another thread of the group and waits for them (W0: no spin lock held;
// the kernel lock released meanwhile). An exit wanted while an exec runs
// sets pending_exit and returns lost (exit beats exec at the exec's commit).
// An exit claim stores in `owed` the status of a kill accepted before it.
template <QuiesceHost Host>
QuiesceResult quiesce_group(SchedulerState &scheduler, ProcessId pid, ThreadId claimer, GroupTransition kind,
                            Host &host, ExitRequest request = {}, std::optional<ExitRequest> *owed = nullptr)
{
    const auto claim = scheduler.claim_transition(pid, claimer, kind, request);
    if (owed && claim.owed) *owed = claim.owed;
    switch (claim.outcome)
    {
    case SchedulerState::ClaimOutcome::claimed:
        break;
    case SchedulerState::ClaimOutcome::held:
        return QuiesceResult::quiesced;
    case SchedulerState::ClaimOutcome::departed:
        return QuiesceResult::departed;
    default:
        return QuiesceResult::lost;
    }
    const auto self = smp::current_cpu();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if (cpu != self && (claim.kick & (1u << cpu)) != 0) smp::send_ipi(cpu, smp::ipi_kick);
    wait_group_quiesced(scheduler, pid, claimer, host);
    return QuiesceResult::quiesced;
}

} // namespace ok::sched
