#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_section.hpp"
#include "ok/smp/lock.hpp"
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
    // Odd while this CPU's kernel code walks user translations under
    // `kernel_root` (0: any root): a kernel window around a user copy, a range
    // check or one user word. Waited for like a user window, never kicked.
    std::atomic<u32> kernel_generation{};
    std::atomic<uptr> kernel_root{};
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
// Bring-up stores it under the kernel lock; a CPU that leaves (hotplug)
// clears its own bit under that lock, and a restart sets it again.
inline constinit std::atomic<u32> online{1};
// The CPUs bring-up started. Hotplug keeps a parked CPU in it; reports list
// these CPUs, whose counters stay meaningful while one is parked.
inline constinit std::atomic<u32> started{1};
inline constinit std::atomic<usize> discovered{1};
// Kernel windows this CPU has open (only its own CPU touches its entry).
inline constinit std::array<u32, max_cpus> kernel_window_depth{};
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
// Why the latest of those two calls failed, as a report token, empty after a
// success; CPU 0 reads it right after the call. Stop: request, turn-busy,
// not-online, mask-busy, scheduler, park-timeout, leave-fault. Restart:
// request, turn-busy, not-parked, the architecture's start_failure() or
// start-error when start() failed, start-timeout (not online within the
// start timeout), wrong-identity, scheduler, ipi-timeout.
[[nodiscard]] std::string_view transition_failure() noexcept;

// The smp_hotplug_test debug knob: once userland runs, CPU 0's kernel loop
// stops and restarts the highest started secondary periodically.
struct HotplugStatistics
{
    usize cycles{};
    usize failures{};
    CpuId cpu{};
};
[[nodiscard]] HotplugStatistics hotplug_statistics() noexcept;
// A failed cycle of that test: its number (1-based), the step that failed
// (stop, restart, or state: the CPU was not offline to restart) and that
// step's status message. `cycle` is 0 while none failed.
struct HotplugFailure
{
    usize cycle{};
    std::string_view phase{};
    std::string_view reason{};
};
// The first and the latest failed cycle.
[[nodiscard]] HotplugFailure hotplug_failure(bool first) noexcept;
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

// The window protocol (x86_64, aarch64 and rv64 run kernel code on several
// CPUs; elsewhere one CPU runs and quiesce_root returns at once).
//
// A writer, holding the owning process's mm: publishes the translation (or
// marks the address-space slot retiring), bumps the space's TLB generation,
// issues a full fence, calls quiesce_root and only then frees what the old
// translation reached. A reader (user entry, a user copy, a range check,
// with_user_word): reads the handle, opens its window (full fence included),
// validates the handle against the slot, walks, copies or enters, and closes
// the window. Either the writer sees the reader's open window and waits for
// it, or the reader sees the retiring slot or the new translation. A window
// body takes no lock (top halves excepted).

// The current CPU opens and closes a window in which it may run user code
// under `root`, around the entry; close it after the switch back to the
// kernel translation. User translations may stay cached past the window
// (ASIDs, PCIDs): the backend revalidates them at the CPU's next entry into
// that space.
inline void begin_user(uptr root) noexcept
{
    auto &record = cpu_record(current_cpu());
    record.user_root.store(root, std::memory_order_relaxed);
    record.user_generation.fetch_add(1, std::memory_order_acq_rel);
    record.user_entries.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void end_user() noexcept
{
    auto &record = cpu_record(current_cpu());
    record.user_root.store(0, std::memory_order_relaxed);
    record.user_generation.fetch_add(1, std::memory_order_release);
}

// A kernel window around a walk of `root`'s user translations. Windows nest
// on one CPU; the outermost opens and closes the shared record, and a nested
// window on another root widens it to every root. Only this CPU writes its
// record, with loads and stores.
inline void begin_kernel_window(uptr root) noexcept
{
    const auto cpu = current_cpu();
    if (cpu >= max_cpus)
        return;
    auto &record = cpu_record(cpu);
    auto &depth = detail::kernel_window_depth[cpu];
    if (depth++ != 0)
    {
        // Widened before this window validates anything, with the same fence
        // as the outermost one: a writer on `root` then sees it.
        if (record.kernel_root.load(std::memory_order_relaxed) != root)
        {
            record.kernel_root.store(0, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        return;
    }
    record.kernel_root.store(root, std::memory_order_relaxed);
    record.kernel_generation.store(record.kernel_generation.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_relaxed);
    set_cpu_mode(cpu, cpu_mode_window);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    detail::owner_add(evidence_cpus[cpu].kernel_windows);
}

inline void end_kernel_window() noexcept
{
    const auto cpu = current_cpu();
    if (cpu >= max_cpus || detail::kernel_window_depth[cpu] == 0)
        return;
    if (--detail::kernel_window_depth[cpu] != 0)
        return;
    clear_cpu_mode(cpu, cpu_mode_window);
    auto &record = cpu_record(cpu);
    record.kernel_generation.store(record.kernel_generation.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_release);
}

[[nodiscard]] inline bool in_kernel_window() noexcept
{
    const auto cpu = current_cpu();
    return cpu < max_cpus && detail::kernel_window_depth[cpu] != 0;
}

class KernelWindow final
{
  public:
    explicit KernelWindow(uptr root) noexcept
    {
        begin_kernel_window(root);
    }
    ~KernelWindow()
    {
        end_kernel_window();
    }
    KernelWindow(const KernelWindow &) = delete;
    KernelWindow &operator=(const KernelWindow &) = delete;
};

// Before a translation under `root` changes or its tables are freed, after
// the change was published and the TLB generation bumped: send every other
// CPU whose user window uses `root` (all windows for root 0) out of user mode
// and wait until that window closed; wait, without a kick, for every kernel
// window on `root` too. The waiting CPU holds nothing above mm (W2) and serves
// its own interrupts meanwhile. CPUs outside a window are never waited for;
// what they cached earlier is the backend's to invalidate at their next
// entry. A CPU that leaves the online mask meanwhile (hotplug) closed its
// windows before, so it is not waited for either. Returns the CPUs waited for.
inline u32 quiesce_root(uptr root) noexcept
{
    static_cast<void>(check_class_wait("quiesce_root under a spin lock"));
    if (lock_debug_holds_above(current_cpu(), LockRank::mm))
        detail::section_assert(&SectionCpu::depth_asserts, "quiesce_root above mm");
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const auto mask = online_mask();
    if ((mask & (mask - 1)) == 0)
        return 0;
    const auto self = current_cpu();
    std::array<u32, max_cpus> user_observed{};
    std::array<u32, max_cpus> kernel_observed{};
    u32 user_waiting = 0;
    u32 kernel_waiting = 0;
    for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
    {
        if (cpu == self || (mask & (u32{1} << cpu)) == 0)
            continue;
        auto &record = cpu_record(cpu);
        const auto kernel = record.kernel_generation.load(std::memory_order_acquire);
        const auto kernel_root = record.kernel_root.load(std::memory_order_relaxed);
        if ((kernel & 1) != 0 && (root == 0 || kernel_root == 0 || kernel_root == root))
        {
            kernel_observed[cpu] = kernel;
            kernel_waiting |= u32{1} << cpu;
        }
        const auto generation = record.user_generation.load(std::memory_order_acquire);
        if ((generation & 1) == 0 || (root != 0 && record.user_root.load(std::memory_order_relaxed) != root))
            continue;
        user_observed[cpu] = generation;
        user_waiting |= u32{1} << cpu;
        send_ipi(cpu, ipi_kick);
    }
    const auto waited = user_waiting | kernel_waiting;
    if (waited != 0)
    {
        cpu_record(self).shootdowns.fetch_add(1, std::memory_order_relaxed);
        detail::owner_add(evidence_cpus[self < max_cpus ? self : 0].quiesce_rounds);
    }
    if (waited == 0)
        return 0;
    const SpinningScope spinning;
    while ((user_waiting | kernel_waiting) != 0)
    {
        const auto online = online_mask();
        user_waiting &= online;
        kernel_waiting &= online;
        for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
        {
            const auto bit = u32{1} << cpu;
            if ((user_waiting & bit) != 0 &&
                cpu_record(cpu).user_generation.load(std::memory_order_acquire) != user_observed[cpu])
                user_waiting &= ~bit;
            if ((kernel_waiting & bit) != 0 &&
                cpu_record(cpu).kernel_generation.load(std::memory_order_acquire) != kernel_observed[cpu])
                kernel_waiting &= ~bit;
        }
        serve_while_spinning();
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
