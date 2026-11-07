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
    // Completed kernel stretches: every release and every section end.
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

// The caller passes `waited` when an earlier failed try already showed the
// lock held elsewhere (a CPU that serves interrupts while it waits).
inline void bkl_lock(bool waited = false) noexcept
{
    waited = kernel_lock.lock() || waited;
    detail::bkl_acquired(current_cpu(), waited);
}

[[nodiscard]] inline bool bkl_try_lock() noexcept
{
    if (!kernel_lock.try_lock())
        return false;
    detail::bkl_acquired(current_cpu(), false);
    return true;
}

// Counts one completed kernel stretch on `cpu` (its `done` word).
inline void bkl_count_done(CpuId cpu) noexcept
{
    if (cpu < max_cpus)
        detail::owner_add(bkl_cpus[cpu].done);
}

inline void bkl_unlock() noexcept
{
    const auto cpu = current_cpu();
    clear_cpu_mode(cpu, cpu_mode_bkl_held);
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
