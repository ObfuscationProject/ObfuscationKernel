// Several CPUs serve native threads. Each CPU's loop holds the kernel lock
// except while it runs user code; the entry it is in then is recorded here so
// that a CPU retiring that thread can take the entry over.
#include "private.hpp"
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
    smp::kernel_lock.lock();
    const bool revoked = entry.revoked;
    entry = {};
    if (revoked) return Status::no_process("another CPU retired this thread during its user entry");
    return trap;
}

Status evict_thread(Kernel &kernel, const sched::ThreadControlBlock &thread)
{
    const auto self = smp::current_cpu();
    u32 running = 0;
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if (cpu != self && !cpu_entries[cpu].revoked && cpu_entries[cpu].pid == thread.owner &&
            cpu_entries[cpu].tid == thread.tid)
            running |= u32{1} << cpu;
    if (!running) return {};
    // Every CPU in this address space leaves user mode. Each then waits for the
    // kernel lock this CPU holds, so its lease is no longer in use.
    const auto *process = kernel.scheduler().find(thread.owner);
    smp::quiesce_root(process ? process->resources().native_space.root : 0);
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if ((running & (u32{1} << cpu)) == 0) continue;
        auto &entry = cpu_entries[cpu];
        if (auto status = entry_stacks.deactivate_entry(entry.lease, {entry.pid, entry.tid}, cpu); !status)
            return status;
        static_cast<void>(kernel.scheduler().clear_on_cpu(entry.pid, entry.tid, cpu));
        entry.revoked = true;
    }
    return {};
}
} // namespace runtime_detail
} // namespace ok::user::native

// A secondary CPU's share of the native runtime: one step under the kernel
// lock. False when it found nothing to run, so the CPU idles until its local
// timer or an IPI.
extern "C" bool ok_smp_secondary_step(ok::smp::CpuId cpu)
{
    using namespace ok::user::native;
    if (!secondary_admission.load(std::memory_order_acquire)) return false;
    ok::smp::RankedSpinLock<ok::smp::LockRank::big_kernel>::Guard guard(ok::smp::kernel_lock);
    if (!started || runtime_kernel == nullptr) return false;
    const bool stepped = step(*runtime_kernel);
    return stepped && !runtime_idle[cpu];
}
