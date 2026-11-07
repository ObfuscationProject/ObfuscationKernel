#pragma once

#include "ok/smp/lock.hpp"

#include <array>
#include <atomic>

namespace ok::smp
{

// The big kernel lock: every CPU's kernel loop holds it while it touches
// kernel state (scheduler, runtime tables, VFS, allocators, device bottom
// halves) and releases it only while that CPU runs user code, idles or runs a
// kernel section (ok/smp/kernel_section.hpp). User code of different threads
// therefore runs concurrently; other kernel work is serialized. Finer locks
// rank after it. Take and release it only through bkl_lock, bkl_try_lock,
// bkl_unlock and BklGuard, which keep this CPU's mode bits and accounting.
inline constinit RankedSpinLock<LockRank::big_kernel> kernel_lock{};

// One CPU's kernel lock accounting. Only that CPU writes it (a load and a
// store, no read-modify-write atomic); the report sums the CPUs.
struct alignas(64) BklCpu
{
    std::atomic<uptr> acquired{};
    // Acquisitions that found the lock held elsewhere and waited.
    std::atomic<uptr> contended{};
    // Completed kernel work: every release of a stretch marked with
    // bkl_mark_work, and every section end. An idle scan that found nothing
    // to serve does not count.
    std::atomic<uptr> done{};
};
inline constinit std::array<BklCpu, max_cpus> bkl_cpus{};

namespace detail
{
inline void owner_add(std::atomic<uptr> &counter, uptr amount = 1) noexcept
{
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

inline void bkl_acquired(CpuId cpu, bool waited) noexcept
{
    if (cpu >= max_cpus)
        return;
    owner_add(bkl_cpus[cpu].acquired);
    if (waited)
        owner_add(bkl_cpus[cpu].contended);
    set_cpu_mode(cpu, cpu_mode_bkl_held);
}
} // namespace detail

inline void bkl_lock() noexcept
{
    const bool waited = kernel_lock.lock();
    detail::bkl_acquired(current_cpu(), waited);
}

// `waited`: earlier failed tries of this acquisition (a CPU that serves its
// interrupts between tries) count it as contended.
[[nodiscard]] inline bool bkl_try_lock(bool waited = false) noexcept
{
    if (!kernel_lock.try_lock())
        return false;
    detail::bkl_acquired(current_cpu(), waited);
    return true;
}

// Counts one completed kernel stretch on `cpu` (its `done` word).
inline void bkl_count_done(CpuId cpu) noexcept
{
    if (cpu < max_cpus)
        detail::owner_add(bkl_cpus[cpu].done);
}

// Marks this CPU's current kernel-lock stretch as work for the overlap
// evidence (ok/smp/kernel_section.hpp): it serves a thread, a trap, a device's
// bottom halves or a section result. Without the lock it does nothing.
inline void bkl_mark_work() noexcept
{
    if (kernel_lock.owned_by_current_cpu())
        set_cpu_mode(current_cpu(), cpu_mode_kernel_work);
}

inline void bkl_unlock() noexcept
{
    const auto cpu = current_cpu();
    const bool worked = (cpu_mode(cpu) & cpu_mode_kernel_work) != 0;
    clear_cpu_mode(cpu, cpu_mode_bkl_held | cpu_mode_kernel_work);
    if (worked)
        bkl_count_done(cpu);
    kernel_lock.unlock();
}

class BklGuard final
{
  public:
    BklGuard() noexcept
    {
        bkl_lock();
    }
    ~BklGuard()
    {
        bkl_unlock();
    }

    BklGuard(const BklGuard &) = delete;
    BklGuard &operator=(const BklGuard &) = delete;
};

} // namespace ok::smp
