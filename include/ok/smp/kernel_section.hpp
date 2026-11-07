#pragma once

#include "ok/smp/kernel_lock.hpp"

#include <array>
#include <atomic>
#include <string_view>

namespace ok::smp
{

// Kernel sections: a CPU runs one admitted syscall body with the big kernel
// lock released. It touches only its own process's footprint (read-only),
// state behind fine locks and records of its own CPU.
//
// Protocol, per CPU c (record section_records[c]):
//   idle -> working     enter_section, holding the kernel lock (the pin); then
//                       c releases the lock and runs the body.
//   working -> returning leave_section publishes the result and retakes the lock.
//   returning -> committed  under the kernel lock: by c, or by a drainer that
//                       found the record first (a handoff).
//   committed -> idle   c, holding the kernel lock again.
// A pin is set only while holding the kernel lock, so a CPU that holds it
// and drains (drain, drain_all) sees every section that can still touch a
// footprint, waits for the working ones and commits the returning ones
// through section_commit_handler before it mutates anything.
//
// Mode bits (cpu_modes, ok/smp/lock.hpp): BKL_HELD, SECTION, SPINNING.
inline constexpr u32 mode_bkl_held = cpu_mode_bkl_held;
inline constexpr u32 mode_section = cpu_mode_section;
inline constexpr u32 mode_spinning = cpu_mode_spinning;

// Doing kernel work right now: holding the lock or in a section, not waiting.
[[nodiscard]] inline bool productive(CpuId cpu) noexcept
{
    const auto bits = cpu_mode(cpu);
    return (bits & (mode_bkl_held | mode_section)) != 0 && (bits & mode_spinning) == 0;
}

[[nodiscard]] inline bool in_section(CpuId cpu) noexcept
{
    return (cpu_mode(cpu) & mode_section) != 0;
}

[[nodiscard]] inline bool in_section() noexcept
{
    return in_section(current_cpu());
}

enum class SectionState : u32
{
    idle,
    working,
    returning,
    committed,
};

// One CPU's section. `pid`, `tid` and the snapshots are written by the owner
// while idle and holding the kernel lock; `result` by the owner before
// `returning`. Others read them only after an acquire load of `state`.
struct alignas(64) SectionRecord
{
    std::atomic<u32> state{};
    u64 pid{};
    u64 tid{};
    i64 result{};
    // Evidence snapshots taken at the pin.
    uptr spins{};
    std::array<uptr, max_cpus> done{};

    [[nodiscard]] SectionState load(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return static_cast<SectionState>(state.load(order));
    }
    void store(SectionState value, std::memory_order order = std::memory_order_release) noexcept
    {
        state.store(static_cast<u32>(value), order);
    }
};
inline constinit std::array<SectionRecord, max_cpus> section_records{};

// Converted families and decline reasons, in report order.
enum class SectionFamily : u8
{
    getpid,
    gettid,
    clock_gettime,
    clock_getres,
    read,
    write,
    pread64,
    pwrite64,
    lseek,
    fsync,
    fdatasync,
    count,
};
inline constexpr usize section_family_count = static_cast<usize>(SectionFamily::count);
inline constexpr std::array<std::string_view, section_family_count> section_family_names{
    "getpid", "gettid", "clock_gettime", "clock_getres", "read", "write",
    "pread64", "pwrite64", "lseek", "fsync", "fdatasync"};

enum class SectionDecline : u8
{
    runtime,
    personality,
    number,
    pending,
    console,
    descriptor,
    kind,
    append,
    size,
    fault,
    shadow,
    shutdown,
    count,
};
inline constexpr usize section_decline_count = static_cast<usize>(SectionDecline::count);
inline constexpr std::array<std::string_view, section_decline_count> section_decline_names{
    "runtime", "personality", "number", "pending", "console",
    "descriptor", "kind", "append", "size", "fault", "shadow", "shutdown"};

// One CPU's section counters. Only that CPU writes them (detail::owner_add);
// section_totals sums the CPUs.
struct alignas(64) SectionCpu
{
    std::atomic<uptr> sections{};     // Pins taken.
    std::atomic<uptr> completed{};    // Sections back under the kernel lock.
    std::atomic<uptr> declined{};
    std::atomic<uptr> handoffs{};     // Owner found its result committed by a drainer.
    std::atomic<uptr> concurrent{};   // Non-spinning sections that saw another CPU productive.
    std::atomic<uptr> witnessed{};    // Non-spinning sections during which another CPU's `done` moved.
    std::atomic<uptr> max_parallel{}; // Highest 1 + productive others at a sample.
    std::atomic<uptr> spun{};         // Sections that waited for a lock, gate or device.
    std::atomic<uptr> drains{};
    std::atomic<uptr> drain_waits{};  // Drains that waited for a working section.
    std::atomic<uptr> exclusive_asserts{};
    std::atomic<uptr> drain_asserts{};
    std::atomic<uptr> depth_asserts{};
    std::atomic<uptr> section_asserts{};
    std::atomic<uptr> cow_refused{};
    std::atomic<uptr> purity_faults{};
    std::atomic<uptr> shadow_checked{};
    std::atomic<uptr> shadow_mismatched{};
    std::array<std::atomic<uptr>, section_family_count> families{};
    std::array<std::atomic<uptr>, section_decline_count> declines{};
    // The first failed assertion's site on this CPU, for the report.
    std::atomic<const char *> first_site{};
};
inline constinit std::array<SectionCpu, max_cpus> section_cpus{};

// The runtime closes admission before power-off; WP3's admission reads it.
inline constinit std::atomic<bool> section_admission{};
// require_exclusive checks the kernel lock once secondary CPUs are admitted.
inline constinit std::atomic<bool> exclusive_armed{};

// Commits a returning section's result under the kernel lock; installed at
// runtime start. The record is already `committed` when it is called.
using SectionCommitHandler = void (*)(SectionRecord &) noexcept;
inline constinit SectionCommitHandler section_commit_handler{};

namespace detail
{
[[nodiscard]] inline SectionCpu *section_cpu(CpuId cpu) noexcept
{
    return cpu < max_cpus ? &section_cpus[cpu] : nullptr;
}

inline void section_count(std::atomic<uptr> SectionCpu::*counter, uptr amount = 1) noexcept
{
    if (auto *self = section_cpu(current_cpu()))
        owner_add(self->*counter, amount);
}

inline void section_assert(std::atomic<uptr> SectionCpu::*counter, const char *site) noexcept
{
    auto *self = section_cpu(current_cpu());
    if (self == nullptr)
        return;
    owner_add(self->*counter);
    if (self->first_site.load(std::memory_order_relaxed) == nullptr)
        self->first_site.store(site, std::memory_order_relaxed);
}

inline void section_commit(SectionRecord &record) noexcept
{
    record.store(SectionState::committed);
    if (const auto handler = section_commit_handler; handler != nullptr)
        handler(record);
    else
        section_assert(&SectionCpu::section_asserts, "section commit without a handler");
}

template <bool All> inline void drain(u64 pid) noexcept
{
    const auto self = current_cpu();
    bool waited = false;
    for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
    {
        if (cpu == self)
            continue;
        auto &record = section_records[cpu];
        auto state = record.load();
        if (state == SectionState::idle || state == SectionState::committed || (!All && record.pid != pid))
            continue;
        if (state == SectionState::working)
        {
            // The pin needs the kernel lock this CPU holds: the section can
            // only end, in bounded time.
            waited = true;
            const SpinningScope spinning;
            while ((state = record.load()) == SectionState::working)
                arch::ArchOperations::spin_pause();
        }
        if (state == SectionState::returning)
            section_commit(record);
    }
    section_count(&SectionCpu::drains);
    if (waited)
        section_count(&SectionCpu::drain_waits);
}
} // namespace detail

inline void open_section_admission() noexcept
{
    section_admission.store(true, std::memory_order_release);
}

inline void close_section_admission() noexcept
{
    section_admission.store(false, std::memory_order_release);
}

[[nodiscard]] inline bool section_admission_open() noexcept
{
    return section_admission.load(std::memory_order_acquire);
}

inline void arm_exclusive() noexcept
{
    exclusive_armed.store(true, std::memory_order_release);
}

// Kernel state outside a section's footprint: refused inside a section and,
// once armed, without the kernel lock. False (and counted) on a violation.
inline bool require_exclusive(const char *site) noexcept
{
    const bool exclusive = !in_section() && (!exclusive_armed.load(std::memory_order_relaxed) ||
                                             kernel_lock.owned_by_current_cpu());
    if (!exclusive)
        detail::section_assert(&SectionCpu::exclusive_asserts, site);
    return exclusive;
}

// A mutator of `pid`'s footprint: no section of it may be working or returning.
inline bool require_drained(u64 pid, const char *site) noexcept
{
    for (const auto &record : section_records)
    {
        const auto state = record.load();
        if ((state == SectionState::working || state == SectionState::returning) && record.pid == pid)
        {
            detail::section_assert(&SectionCpu::drain_asserts, site);
            return false;
        }
    }
    return true;
}

// The ranked locks this CPU holds, where OK_LOCK_DEBUG records them.
inline bool require_lock_depth([[maybe_unused]] usize expected, [[maybe_unused]] const char *site) noexcept
{
#if defined(OK_LOCK_DEBUG)
    if (lock_debug_depth(current_cpu()) != expected)
    {
        detail::section_assert(&SectionCpu::depth_asserts, site);
        return false;
    }
#endif
    return true;
}

// Holding only the kernel lock: waits for `pid`'s working sections on other
// CPUs and commits their returning ones.
inline void drain(u64 pid) noexcept
{
    require_lock_depth(1, "drain");
    detail::drain<false>(pid);
}

// The same for every process.
inline void drain_all() noexcept
{
    require_lock_depth(1, "drain_all");
    detail::drain<true>(0);
}

// Pins a section for (pid, tid) on this CPU and releases the kernel lock,
// which the caller holds (and nothing else).
inline void enter_section(u64 pid, u64 tid) noexcept
{
    const auto self = current_cpu();
    require_lock_depth(1, "section start");
    if (self >= max_cpus)
        return;
    auto &record = section_records[self];
    record.pid = pid;
    record.tid = tid;
    record.result = 0;
    record.spins = cpu_modes[self].spins.load(std::memory_order_relaxed);
    for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
        record.done[cpu] = bkl_cpus[cpu].done.load(std::memory_order_relaxed);
    record.store(SectionState::working);
    set_cpu_mode(self, mode_section);
    detail::owner_add(section_cpus[self].sections);
    bkl_unlock();
    require_lock_depth(0, "section unlocked");
}

// Ends this CPU's section with `result` and returns holding the kernel lock,
// once the result is committed (here or by a drainer). `relock` takes the
// kernel lock: bkl_lock, or a variant that serves device lines meanwhile.
template <typename Relock> inline void leave_section(i64 result, Relock &&relock) noexcept
{
    const auto self = current_cpu();
    require_lock_depth(0, "section end");
    if (self >= max_cpus)
        return;
    auto &record = section_records[self];
    auto &stats = section_cpus[self];
    // Overlap evidence only for a section that never waited, against CPUs
    // that are not waiting either.
    if (cpu_modes[self].spins.load(std::memory_order_relaxed) == record.spins)
    {
        uptr others = 0;
        bool moved = false;
        for (CpuId cpu = 0; cpu < max_cpus; ++cpu)
        {
            if (cpu == self)
                continue;
            others += productive(cpu) ? 1 : 0;
            moved = moved || bkl_cpus[cpu].done.load(std::memory_order_relaxed) != record.done[cpu];
        }
        if (others != 0)
            detail::owner_add(stats.concurrent);
        if (moved)
            detail::owner_add(stats.witnessed);
        if (others + 1 > stats.max_parallel.load(std::memory_order_relaxed))
            stats.max_parallel.store(others + 1, std::memory_order_relaxed);
    }
    else
        detail::owner_add(stats.spun);
    record.result = result;
    clear_cpu_mode(self, mode_section);
    bkl_count_done(self);
    record.store(SectionState::returning);
    relock();
    if (record.load() == SectionState::committed)
        detail::owner_add(stats.handoffs);
    else
        detail::section_commit(record);
    record.store(SectionState::idle, std::memory_order_relaxed);
    detail::owner_add(stats.completed);
}

inline void leave_section(i64 result) noexcept
{
    leave_section(result, [] { bkl_lock(); });
}

inline void count_section_family(SectionFamily family) noexcept
{
    if (auto *self = detail::section_cpu(current_cpu()))
        detail::owner_add(self->families[static_cast<usize>(family)]);
}

inline void count_section_decline(SectionDecline reason) noexcept
{
    if (auto *self = detail::section_cpu(current_cpu()))
    {
        detail::owner_add(self->declined);
        detail::owner_add(self->declines[static_cast<usize>(reason)]);
    }
}

} // namespace ok::smp
