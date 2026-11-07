#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <string_view>

namespace ok { class Kernel; }

namespace ok::smp
{

// Reasons accumulate in the target's record until it drains them.
enum IpiReason : u32
{
    ipi_kick = 1,       // leave user mode: a shootdown or remote state change
    ipi_reschedule = 2, // work was queued for this CPU
    ipi_ping = 4,       // bring-up delivery check
    ipi_device = 8,     // a device line or event source this CPU waits for became pending
};

// What one CPU publishes for the others. `reported_id` is read by that CPU from
// its own registers once it runs kernel code; firmware tables only supply
// `hardware_id`, the identity it was started with.
struct alignas(64) CpuRecord
{
    std::atomic<u8> state{static_cast<u8>(CpuState::offline)};
    u64 hardware_id{};
    std::atomic<uptr> reported_id{~uptr{0}};
    uptr stack_lower{};
    uptr stack_top{};
    // Odd while this CPU may execute user code under `user_root`. A CPU that
    // changes a translation waits for every such window to close.
    std::atomic<u32> user_generation{};
    std::atomic<uptr> user_root{};
    std::atomic<u32> ipi_pending{};
    std::atomic<uptr> ipis_sent{};
    std::atomic<uptr> ipis_received{};
    std::atomic<uptr> local_ticks{};
    std::atomic<uptr> user_entries{};
    std::atomic<uptr> shootdowns{};
};

struct BringupReport
{
    usize discovered{1};
    usize online{1};
    std::string_view method{"unsupported"};
    std::string_view ipi{"unsupported"};
    std::string_view reason{};
};

namespace detail
{
inline constinit std::array<CpuRecord, max_cpus> records{};
// Changed only under the kernel lock: bring-up stores it, a CPU that leaves
// (hotplug) clears its own bit and a restart sets it again.
inline constinit std::atomic<u32> online{1};
// The CPUs bring-up started. Hotplug keeps a parked CPU in it; reports list
// these CPUs, whose counters stay meaningful while one is parked.
inline constinit std::atomic<u32> started{1};
inline constinit std::atomic<usize> discovered{1};
// Bring-up installs the architecture's IPI sender; until then, and in hosted
// models, nothing is sent.
inline constinit void (*ipi_sender)(CpuId cpu, u64 hardware_id) noexcept {};
// Ends the receiving CPU's interrupt wait (the interrupt service installs it).
inline constinit void (*ipi_wake)() noexcept {};
} // namespace detail

[[nodiscard]] inline CpuRecord &cpu_record(CpuId cpu) noexcept
{
    return detail::records[cpu < max_cpus ? cpu : 0];
}
[[nodiscard]] inline usize discovered_cpus() noexcept
{
    return detail::discovered.load(std::memory_order_acquire);
}
[[nodiscard]] inline u32 online_mask() noexcept
{
    return detail::online.load(std::memory_order_acquire);
}
[[nodiscard]] constexpr usize cpu_count(u32 mask) noexcept
{
    usize count = 0;
    for (; mask != 0; mask &= mask - 1)
        ++count;
    return count;
}
[[nodiscard]] inline usize online_cpus() noexcept
{
    return cpu_count(online_mask());
}
[[nodiscard]] inline u32 started_mask() noexcept
{
    return detail::started.load(std::memory_order_acquire);
}
[[nodiscard]] inline usize started_cpus() noexcept
{
    return cpu_count(started_mask());
}
[[nodiscard]] const BringupReport &bringup_report() noexcept;

// Boot CPU, once: discover the CPUs, start every enabled secondary on its own
// stack, check that each one receives an IPI, and print the OK_SMP hardware
// line. A profile or machine that cannot start secondaries reports UP and
// changes nothing else.
Status start_secondary_cpus(Kernel &kernel);

// CPU hotplug of a started secondary (CPU 0 never leaves). The caller holds
// the kernel lock and nothing else, and is not `cpu`; each call releases the
// lock while it waits for `cpu` and returns holding it again, so the caller
// re-resolves any kernel record it kept.
//
// Stop: `cpu` goes stopping, leaves the scheduler's online set (its queue
// moves to online CPUs), its kernel sections are drained and it is kicked out
// of user mode. At its next loop pass outside user code, holding the kernel
// lock, the CPU itself moves its device lines away and clears its online bit;
// then, holding nothing, it drains its IPIs, stops its local timer, resets its
// per-CPU state and parks (x86: cli;hlt until INIT; aarch64: PSCI CPU_OFF;
// rv64: the boot park loop). Returns once it parked, or fails after a timeout
// with the CPU stopping. Nothing it could still touch is freed.
Status stop_secondary_cpu(Kernel &kernel, CpuId cpu);
// Restart: a parked CPU starts again on its own stack through the
// architecture's start path (x86 repeats the TSC handshake), comes online with
// its reported identity, rejoins the online mask and the scheduler, and must
// answer one IPI.
Status restart_secondary_cpu(Kernel &kernel, CpuId cpu);

// The smp_hotplug_test debug knob: once userland runs, CPU 0's kernel loop
// stops and restarts the highest started secondary periodically.
struct HotplugStatistics
{
    usize cycles{};
    usize failures{};
    CpuId cpu{};
};
[[nodiscard]] HotplugStatistics hotplug_statistics() noexcept;
// One pass of that test from CPU 0's kernel loop (kernel lock held, no record
// kept); a no-op unless the knob is built in.
void hotplug_test_pass() noexcept;

inline void send_ipi(CpuId cpu, u32 reasons) noexcept
{
    if (cpu >= max_cpus || (online_mask() & (u32{1} << cpu)) == 0 || detail::ipi_sender == nullptr)
        return;
    auto &record = cpu_record(cpu);
    record.ipi_pending.fetch_or(reasons, std::memory_order_release);
    cpu_record(current_cpu()).ipis_sent.fetch_add(1, std::memory_order_relaxed);
    detail::ipi_sender(cpu, record.hardware_id);
}

// On the receiving CPU, after the architecture acknowledged the interrupt.
// Returns the reasons drained.
inline u32 receive_ipi() noexcept
{
    const auto cpu = current_cpu();
    auto &record = cpu_record(cpu);
    const auto reasons = record.ipi_pending.exchange(0, std::memory_order_acquire);
    record.ipis_received.fetch_add(1, std::memory_order_release);
    if (detail::ipi_wake != nullptr)
        detail::ipi_wake();
    return reasons;
}

// A secondary CPU's local timer expired.
inline void local_tick() noexcept
{
    cpu_record(current_cpu()).local_ticks.fetch_add(1, std::memory_order_relaxed);
}

// The current CPU opens and closes a window in which it may run user code
// under `root`. Open it while holding what a translation change also holds,
// before the entry; close it after the switch back to the kernel translation.
// User translations may stay cached past the window (ASIDs, PCIDs): the
// backend revalidates them at the CPU's next entry into that space.
inline void begin_user(uptr root) noexcept
{
    auto &record = cpu_record(current_cpu());
    record.user_root.store(root, std::memory_order_relaxed);
    record.user_generation.fetch_add(1, std::memory_order_acq_rel);
    record.user_entries.fetch_add(1, std::memory_order_relaxed);
}

inline void end_user() noexcept
{
    auto &record = cpu_record(current_cpu());
    record.user_root.store(0, std::memory_order_relaxed);
    record.user_generation.fetch_add(1, std::memory_order_release);
}

// Before a translation under `root` changes or its tables are freed: send
// every other CPU whose window uses `root` (all windows for root 0) out of
// user mode and wait until that window closed. The waiting CPU must not hold
// anything those CPUs need to leave user mode. CPUs outside a window are never
// waited for; what they cached earlier is the backend's to invalidate at their
// next entry. A CPU that leaves the online mask meanwhile (hotplug) closed its
// window before, so it is not waited for either. Returns the CPUs waited for.
inline u32 quiesce_root(uptr root) noexcept
{
    const auto mask = online_mask();
    if ((mask & (mask - 1)) == 0)
        return 0;
    const auto self = current_cpu();
    std::array<u32, max_cpus> observed{};
    u32 waiting = 0;
    for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
    {
        if (cpu == self || (mask & (u32{1} << cpu)) == 0)
            continue;
        auto &record = cpu_record(cpu);
        const auto generation = record.user_generation.load(std::memory_order_acquire);
        if ((generation & 1) == 0 || (root != 0 && record.user_root.load(std::memory_order_relaxed) != root))
            continue;
        observed[cpu] = generation;
        waiting |= u32{1} << cpu;
        send_ipi(cpu, ipi_kick);
    }
    const auto waited = waiting;
    if (waiting != 0)
        cpu_record(self).shootdowns.fetch_add(1, std::memory_order_relaxed);
    while (waiting != 0)
    {
        waiting &= online_mask();
        for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
            if ((waiting & (u32{1} << cpu)) != 0 &&
                cpu_record(cpu).user_generation.load(std::memory_order_acquire) != observed[cpu])
                waiting &= ~(u32{1} << cpu);
        arch::ArchOperations{}.spin_pause();
    }
    return waited;
}

} // namespace ok::smp

// Architecture secondary entry calls this on the new CPU's own stack, with its
// per-CPU registers, translation and interrupt controller interface set up.
extern "C" [[noreturn]] void ok_smp_secondary_main(ok::smp::CpuId cpu);
// The native runtime's work for a secondary CPU, once it serves user threads.
// Returns false when there was nothing to run.
extern "C" bool ok_smp_secondary_step(ok::smp::CpuId cpu) __attribute__((weak));
// The native runtime's part of a secondary CPU leaving (hotplug): on that CPU,
// under the kernel lock, between steps. Clears its per-CPU runtime state and
// vacates its scheduler slot; false when a user entry was still recorded.
extern "C" bool ok_smp_cpu_leaving(ok::smp::CpuId cpu) __attribute__((weak));
