// Several CPUs serve native threads. Each CPU's loop holds the kernel lock
// except while it runs user code; the entry it is in then is recorded here so
// that a CPU retiring that thread can take the entry over.
#include "private.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_lock.hpp"

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
    smp::kernel_lock.unlock();
    auto trap = hw::enter_owned(space, frame, view);
    smp::end_user();
    // The holder may wait for a device line routed to this CPU.
    interrupt::lock_kernel_serving_interrupts();
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
        if (ok::interrupt::platform_irq_pending() && ok::smp::kernel_lock.try_lock())
        {
            static_cast<void>(ok::interrupt::drain_platform_irqs());
            ok::smp::kernel_lock.unlock();
        }
        return false;
    }
    ok::interrupt::lock_kernel_serving_interrupts();
    struct Unlock
    {
        ~Unlock() { ok::smp::kernel_lock.unlock(); }
    } unlock;
    static_cast<void>(ok::interrupt::drain_platform_irqs());
    if (!started || runtime_kernel == nullptr) return false;
    const bool stepped = step(*runtime_kernel);
    return stepped && !runtime_idle[cpu];
}
