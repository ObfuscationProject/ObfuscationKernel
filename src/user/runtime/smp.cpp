// Several CPUs serve native threads. Each CPU's loop holds the kernel lock
// except while it runs user code; the entry it is in then is recorded here so
// that a CPU retiring that thread can take the entry over.
#include "private.hpp"
#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_section.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
struct CpuEntry
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    memory::KernelEntryStackHandle lease{};
    bool revoked{};
};
constinit std::array<CpuEntry, smp::max_cpus> cpu_entries{};
// A backend with fixed slot storage holds every process's space, every
// quarantined one and an exec candidate per CPU.
static_assert(arch::execution::fixed_space_slots ==
              sched::max_processes + std::tuple_size_v<decltype(quarantined_spaces)> + smp::max_cpus);
} // namespace

// Set once the runtime serves threads; secondary CPUs only then contend for
// the kernel lock, and never while bring-up still pings them.
constinit std::atomic<bool> secondary_admission{};
constinit std::array<bool, smp::max_cpus> runtime_step_active{};
constinit std::array<bool, smp::max_cpus> runtime_idle{};

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
                                memory::KernelEntryStackHandle lease, u64 pid, u64 tid)
{
    auto &entry = cpu_entries[smp::current_cpu()];
    entry = {pid, tid, lease, false};
    smp::begin_user(space.root);
    smp::bkl_unlock();
    auto trap = hw::enter_owned(space, frame, view);
    smp::end_user();
    // The holder may wait for a device line routed to this CPU.
    interrupt::lock_kernel_serving_interrupts();
    // This stretch serves the trap.
    smp::bkl_mark_work();
    const bool revoked = entry.revoked;
    entry = {};
    if (revoked) return Status::no_process("another CPU retired this thread during its user entry");
    return trap;
}

Status prepare_eviction(sched::ProcessId pid, uptr root, std::span<const sched::ThreadControlBlock> threads,
                        EvictionPlan &plan)
{
    plan = {pid, root, 0};
    const auto self = smp::current_cpu();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        const auto &entry = cpu_entries[cpu];
        if (cpu == self || entry.revoked || entry.pid != pid) continue;
        const sched::ThreadControlBlock *thread{};
        for (const auto &member : threads)
            if (member.owner == pid && member.tid == entry.tid) thread = &member;
        if (!thread) continue;
        // The pool has this lease active on that CPU, so deactivating it
        // cannot be refused.
        if (auto status = entry_stacks.validate_active(entry.lease, {entry.pid, entry.tid}, cpu); !status)
            return status;
        plan.cpus |= u32{1} << cpu;
    }
    return {};
}

Status validate_evicted_leases(std::span<const sched::ThreadControlBlock> threads, const EvictionPlan &plan)
{
    for (const auto &thread : threads)
    {
        if (!thread.entry_stack.pool) continue;
        bool planned = false;
        for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        {
            if ((plan.cpus & (u32{1} << cpu)) == 0 || cpu_entries[cpu].tid != thread.tid) continue;
            // The CPU runs the thread on the lease its TCB names.
            if (cpu_entries[cpu].lease != thread.entry_stack)
                return Status::fault("CPU entry does not name its thread's lease");
            planned = true;
        }
        if (!planned)
            if (auto status = entry_stacks.validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
                return status;
    }
    return {};
}

Status commit_eviction(Kernel &kernel, const EvictionPlan &plan)
{
    if (!plan.cpus) return {};
    // Every CPU in this address space leaves user mode. Each then waits for the
    // kernel lock this CPU holds, so its lease is no longer in use.
    smp::require_exclusive("commit_eviction");
    smp::quiesce_root(plan.root);
    Status failure{};
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if ((plan.cpus & (u32{1} << cpu)) == 0) continue;
        auto &entry = cpu_entries[cpu];
        // A damaged lease is still revoked and quarantined; finish the others.
        if (auto status = entry_stacks.deactivate_entry(entry.lease, {entry.pid, entry.tid}, cpu); !status && failure)
            failure = status;
        static_cast<void>(kernel.scheduler().clear_on_cpu(entry.pid, entry.tid, cpu));
        entry.revoked = true;
    }
    return failure;
}

Status evict_thread(Kernel &kernel, const sched::ThreadControlBlock &thread)
{
    const auto *process = kernel.scheduler().find(thread.owner);
    EvictionPlan plan;
    if (auto status = prepare_eviction(thread.owner, process ? process->resources().native_space.root : 0,
                                       {&thread, 1}, plan); !status)
        return status;
    return commit_eviction(kernel, plan);
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
    const bool clean = cpu_entries[cpu].pid == 0 && !cpu_entries[cpu].lease.pool && !runtime_step_active[cpu] &&
                       !entry_stacks.active_on(cpu);
    cpu_entries[cpu] = {};
    runtime_step_active[cpu] = false;
    runtime_idle[cpu] = false;
    forget_cpu_cursor(cpu);
    if (runtime_kernel != nullptr && runtime_kernel->scheduler().current_tid(cpu) != 0)
        static_cast<void>(runtime_kernel->scheduler().activate(0, cpu));
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
    return stepped && !runtime_idle[cpu];
}
