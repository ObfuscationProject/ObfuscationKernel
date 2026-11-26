// Several CPUs serve native threads. Each CPU's loop holds the kernel lock
// except while it runs user code. A group transition (exec, exit_group)
// freezes the other threads of its process and waits, with the kernel lock
// released, until each left its CPU through its own put_prev (spec 4.8).
#include "private.hpp"
#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/sched/group_quiesce.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_section.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
// A backend with fixed slot storage holds every process's space, every
// quarantined one and an exec candidate per CPU.
static_assert(arch::execution::fixed_space_slots ==
              sched::max_processes + std::tuple_size_v<decltype(quarantined_spaces)> + smp::max_cpus);

// The runtime's side of quiesce_group: the kernel lock (released while the
// claimer waits, taken again serving this CPU's device lines), this CPU's
// interrupts and the monotonic clock.
struct RuntimeQuiesceHost
{
    bool kernel_lock_held() const { return smp::kernel_lock.owned_by_current_cpu(); }
    void release_kernel_lock() { smp::bkl_unlock(); }
    void retake_kernel_lock()
    {
        interrupt::lock_kernel_serving_interrupts();
        smp::bkl_mark_work();
    }
    void serve() { interrupt::serve_local_interrupts(); }
    u64 now_ns() const
    {
        const auto now = arch::monotonic_nanoseconds();
        return now ? now.value() : 0;
    }
};
static_assert(sched::QuiesceHost<RuntimeQuiesceHost>);

void kick(u32 cpus)
{
    const auto self = smp::current_cpu();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if (cpu != self && (cpus & (1u << cpu)) != 0) smp::send_ipi(cpu, smp::ipi_kick);
}
} // namespace

// Set once the runtime serves threads; secondary CPUs only then contend for
// the kernel lock, and never while bring-up still pings them.
constinit std::atomic<bool> secondary_admission{};
constinit std::array<bool, smp::max_cpus> runtime_step_active{};

Status require_cpu_service_stack()
{
#ifdef OK_KERNEL_FREESTANDING
    const auto cpu = smp::current_cpu();
    if (cpu == 0)
        return arch::require_service_stack();
    const auto &record = smp::cpu_record(cpu);
    return arch::StackBounds{record.stack_lower, record.stack_top}.require(arch::read_instantaneous_stack_pointer(), 0);
#else
    return {};
#endif
}

bool any_step_active()
{
    for (const bool active : runtime_step_active)
        if (active) return true;
    return false;
}

Result<hw::Trap> enter_unlocked(hw::AddressSpace &space, hw::UserFrame &frame, const hw::KernelEntryStackView &view,
                                memory::KernelEntryStackHandle, u64, u64)
{
    smp::begin_user(space.root);
    smp::bkl_unlock();
    auto trap = hw::enter_owned(space, frame, view);
    smp::end_user();
    // The holder may wait for a device line routed to this CPU. A group
    // transition of this thread's process may hold its group frozen: the
    // step asks after its capture (smp::bkl_thread_frozen).
    interrupt::lock_kernel_serving_interrupts();
    // This stretch serves the trap.
    smp::bkl_mark_work();
    return trap;
}

bool group_frozen(u64 pid) noexcept
{
    return runtime_kernel != nullptr && runtime_kernel->scheduler().frozen(pid);
}

sched::QuiesceResult quiesce_group(Kernel &kernel, sched::ProcessId pid, sched::ThreadId claimer,
                                   sched::GroupTransition kind, sched::ExitRequest request)
{
    RuntimeQuiesceHost host;
    return sched::quiesce_group(kernel.scheduler(), pid, claimer, kind, host, request);
}

void request_group_exit(Kernel &kernel, sched::ProcessId pid, sched::ExitRequest request)
{
    kick(kernel.scheduler().request_group_exit(pid, request));
}

bool quiesce_for_exit(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal)
{
    const auto cpu = smp::current_cpu();
    const sched::ExitRequest request{code, signal};
    const auto &slot = kernel.scheduler().cpu_slot(cpu < smp::max_cpus ? cpu : 0);
    const bool own = cpu < smp::max_cpus && slot.occupied() && slot.pid == pid;
    if (cpu < smp::max_cpus && slot.occupied() && !own && slot.process &&
        slot.process->execution() == sched::ProcessExecution::user_process)
    {
        // A thread of another process never waits for this group (spec
        // 4.8). It exits a group none of whose threads is on a CPU (a fork
        // child it undoes) at once; any other group gets a pending exit,
        // which one of its own threads applies at its next pick.
        const auto claim = kernel.scheduler().claim_transition(pid, 0, sched::GroupTransition::exit, request, true);
        switch (claim.outcome)
        {
        case sched::SchedulerState::ClaimOutcome::claimed:
        case sched::SchedulerState::ClaimOutcome::held:
        case sched::SchedulerState::ClaimOutcome::departed:
            return true;
        case sched::SchedulerState::ClaimOutcome::busy:
            request_group_exit(kernel, pid, request);
            return false;
        default:
            return false;
        }
    }
    // A thread of the group claims for itself; a CPU that runs no user
    // thread (its kernel loop, boot) claims for none: nothing waits for it.
    const sched::ThreadId claimer = own ? slot.tid : 0;
    return quiesce_group(kernel, pid, claimer, sched::GroupTransition::exit, request) != sched::QuiesceResult::lost;
}

Status validate_quiesced_leases(std::span<const sched::ThreadControlBlock> threads)
{
    // Every lease of a quiesced group is inactive: each sibling left its CPU
    // through put_prev, after its step deactivated its lease (spec 3.10).
    for (const auto &thread : threads)
        if (thread.entry_stack.pool)
            if (auto status = entry_stacks.validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
                return status;
    return {};
}
} // namespace runtime_detail
} // namespace ok::user::native

// A secondary CPU leaving (hotplug), on that CPU under the kernel lock between
// steps: no user entry may remain, and what the runtime keeps per CPU starts
// fresh when it returns. A thread still in its scheduler slot (a step that
// returned without vacating it) goes back to runnable for the online CPUs.
extern "C" bool ok_smp_cpu_leaving(ok::smp::CpuId cpu)
{
    using namespace ok::user::native;
    if (cpu == 0 || cpu >= ok::smp::max_cpus) return false;
    const bool clean = !runtime_step_active[cpu] && !entry_stacks.active_on(cpu);
    runtime_step_active[cpu] = false;
    forget_cpu_cursor(cpu);
    if (runtime_kernel != nullptr)
    {
        runtime_kernel->scheduler().leave_idle(cpu);
        if (runtime_kernel->scheduler().current_tid(cpu) != 0) runtime_kernel->scheduler().put_prev(cpu);
    }
    return clean;
}

// A secondary CPU's share of the native runtime: one step under the kernel
// lock, after the device bottom halves pending on any CPU. False when it found
// nothing to run, so the CPU idles until its local timer, an IPI or a device
// line routed to it.
extern "C" bool ok_smp_secondary_step(ok::smp::CpuId cpu)
{
    using namespace ok::user::native;
    if (!secondary_admission.load(std::memory_order_acquire))
    {
        // Not serving threads yet: drain a line this CPU took only while the
        // kernel lock is free (the boot CPU idles), since that idle wait does
        // not end for top halves taken here.
        if (ok::interrupt::platform_irq_pending() && ok::smp::bkl_try_lock())
        {
            static_cast<void>(ok::interrupt::drain_platform_irqs());
            ok::smp::bkl_unlock();
        }
        return false;
    }
    ok::interrupt::lock_kernel_serving_interrupts();
    struct Unlock
    {
        ~Unlock() { ok::smp::bkl_unlock(); }
    } unlock;
    static_cast<void>(ok::interrupt::drain_platform_irqs());
    if (!started || runtime_kernel == nullptr) return false;
    const bool stepped = step(*runtime_kernel);
    return stepped && !runtime_kernel->scheduler().cpu_idle(cpu);
}
