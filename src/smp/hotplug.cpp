// The kernel's CPU hotplug service backend (docs/modules/sysfs.md): what
// /sys/devices/system/cpu reads and the CPU 0 pass that serves the writes.
#include "ok/smp/hotplug.hpp"

#include "ok/arch/clock.hpp"
#include "ok/arch/smp.hpp"
#include "ok/core/kernel.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_lock.hpp"

namespace ok::smp
{
namespace
{
static_assert(hotplug::record_capacity == memory::KernelEntryStackPool::capacity,
              "one hotplug record per entry-stack lease");

// Set once bring-up configured the CPUs; the scheduler's online set is part
// of a CPU being online for userland.
constinit Kernel *hotplug_kernel{};

CpuState state(CpuId cpu)
{
    return static_cast<CpuState>(cpu_record(cpu).state.load(std::memory_order_acquire));
}

struct KernelBackend
{
    [[nodiscard]] bool online(CpuId cpu) const { return cpu_online_now(cpu); }
    [[nodiscard]] hotplug::Refusal refusal(CpuId cpu) const
    {
        if (cpu == 0 || cpu >= max_cpus || (started_mask() & (u32{1} << cpu)) == 0)
            return hotplug::Refusal::io_error;
        // The smp_hotplug_test knob owns its CPU.
        if (hotplug_statistics().cpu == cpu || native_shutdown_pending()) return hotplug::Refusal::busy;
        switch (state(cpu))
        {
        case CpuState::stopping:
        case CpuState::starting:
        case CpuState::boot: return hotplug::Refusal::busy;
        case CpuState::halted: return hotplug::Refusal::io_error;
        case CpuState::offline:
        case CpuState::online: break;
        }
        return hotplug::Refusal::none;
    }
    bool owner_waiting(u64 pid, u64 tid, sched::WaitTicket ticket)
    {
        if (!hotplug_kernel) return false;
        // CS2: the writer's slot is pinned and its task lock held, so the
        // process can neither be freed nor change its wait meanwhile.
        const auto pinned = hotplug_kernel->scheduler().pin(pid);
        if (!pinned) return false;
        const sched::TaskGuard guard{*pinned.slot()};
        for (const auto &thread : pinned->live_threads())
            if (thread.tid == tid)
                return thread.state == sched::ProcessState::blocked &&
                       thread.wait_reason == sched::ThreadWaitReason::cpu_hotplug &&
                       thread.wait_registration.pending(ticket);
        return false;
    }
    Status stop(CpuId cpu) { return stop_secondary_cpu(*hotplug_kernel, cpu); }
    Status restart(CpuId cpu) { return restart_secondary_cpu(*hotplug_kernel, cpu); }
    [[nodiscard]] std::string_view failure() const { return transition_failure(); }
    void complete(sched::WaitTicket ticket)
    {
        static_cast<void>(hotplug_kernel->scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    }
    // A stop that timed out: once the CPU parked after all, the topology
    // lists it offline and a later "1" restarts it.
    bool recover([[maybe_unused]] CpuId cpu)
    {
#if defined(OK_ARCH_SMP_START)
        if (state(cpu) != CpuState::offline || !arch::smp::parked(cpu, cpu_record(cpu).hardware_id)) return false;
        static_cast<void>(hotplug_kernel->topology().mark_offline(cpu));
        return true;
#else
        return false;
#endif
    }
    [[nodiscard]] u64 now_ns() const
    {
        const auto now = arch::monotonic_nanoseconds();
        return now ? now.value() : 0;
    }
};
static_assert(hotplug::Backend<KernelBackend>);

constinit hotplug::HotplugService<KernelBackend> service{};
} // namespace

bool cpu_online_now(CpuId cpu) noexcept
{
    if (cpu >= max_cpus) return false;
    const u32 bit = u32{1} << cpu;
    if (state(cpu) != CpuState::online || (online_mask() & bit) == 0) return false;
    return !hotplug_kernel || (hotplug_kernel->scheduler().online_cpus() & bit) != 0;
}

CpuView cpu_view() noexcept
{
    CpuView view{.present = started_mask(), .online = 0};
    // The boot CPU is online before bring-up records its state.
    view.online = 1;
    for (CpuId cpu = 1; cpu < max_cpus; ++cpu)
        if ((view.present & (u32{1} << cpu)) != 0 && cpu_online_now(cpu)) view.online |= u32{1} << cpu;
    return view;
}

void hotplug_attach(Kernel &kernel) noexcept
{
    hotplug_kernel = &kernel;
}

bool hotplug_service_pass() noexcept
{
#if defined(OK_ARCH_SMP_START)
    // On CPU 0, under the kernel lock that stop and restart require.
    if (!hotplug_kernel || current_cpu() != 0 || !kernel_lock.owned_by_current_cpu()) return false;
    KernelBackend backend{};
    return service.pass(backend);
#else
    return false;
#endif
}

namespace hotplug
{
Submission submit(usize slot, const Request &request)
{
    return service.submit(KernelBackend{}, slot, request);
}
std::optional<i64> consume(usize slot, sched::WaitTicket ticket)
{
    return service.consume(slot, ticket);
}
bool finished(usize slot) noexcept
{
    return service.finished(slot);
}
void abandon(usize slot, u64 pid, u64 tid)
{
    service.abandon(slot, pid, tid);
}
u32 pending() noexcept
{
    return service.pending();
}
Statistics statistics() noexcept
{
    return service.statistics();
}
std::optional<Failure> take_failure() noexcept
{
    return service.take_failure();
}
} // namespace hotplug
} // namespace ok::smp
