#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <atomic>

namespace ok::smp
{

using CpuId = u16;
inline constexpr usize max_cpus = 16;

enum class CpuState : u8
{
    offline,
    boot,
    starting,
    online,
    halted,
    // Hotplug: leaving at its next pass outside user code; then offline.
    stopping,
};

enum class TopologyMode : u8
{
    single_core,
    symmetric,
    numa_aware,
};

struct CpuInfo
{
    CpuId id{0};
    CpuState state{CpuState::offline};
    u32 numa_domain{0};
    u64 scheduler_ticks{0};
};

class CpuTopology final
{
  public:
    void set_mode(TopologyMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] TopologyMode mode() const
    {
        return mode_;
    }
    Status initialize(usize cpu_count);
    Status mark_starting(CpuId cpu);
    Status mark_online(CpuId cpu);
    Status mark_halted(CpuId cpu);
    // A CPU parked by hotplug, which may start again.
    Status mark_offline(CpuId cpu);
    Status record_schedule(CpuId cpu);

    [[nodiscard]] usize cpu_count() const
    {
        return cpu_count_;
    }
    [[nodiscard]] usize online_count() const;
    [[nodiscard]] Result<CpuId> least_busy_online_cpu() const;
    [[nodiscard]] const CpuInfo *cpu(CpuId cpu) const;

  private:
    [[nodiscard]] bool valid(CpuId cpu) const
    {
        return cpu < cpu_count_;
    }

    TopologyMode mode_{TopologyMode::single_core};
    std::array<CpuInfo, max_cpus> cpus_{};
    usize cpu_count_{0};
};

template <typename T, usize Capacity = max_cpus> class PerCpu final
{
  public:
    [[nodiscard]] T &get(CpuId cpu)
    {
        return values_[cpu];
    }
    [[nodiscard]] const T &get(CpuId cpu) const
    {
        return values_[cpu];
    }

  private:
    std::array<T, Capacity> values_{};
};

// Unranked leaf lock: it takes no other lock while held. Ranked locks with
// debug order checks live in ok/smp/lock.hpp.
class SpinLock final
{
  public:
    void lock() noexcept
    {
        while (flag_.test_and_set(std::memory_order_acquire))
        {
            while (flag_.test(std::memory_order_relaxed))
            {
                arch::ArchOperations::spin_pause();
            }
        }
    }

    void unlock() noexcept
    {
        flag_.clear(std::memory_order_release);
    }
    [[nodiscard]] bool try_lock() noexcept
    {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

  private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class ScopedSpinLock final
{
  public:
    explicit ScopedSpinLock(SpinLock &lock) : lock_(lock)
    {
        lock_.lock();
    }
    ~ScopedSpinLock()
    {
        lock_.unlock();
    }

    ScopedSpinLock(const ScopedSpinLock &) = delete;
    ScopedSpinLock &operator=(const ScopedSpinLock &) = delete;

  private:
    SpinLock &lock_;
};

} // namespace ok::smp
