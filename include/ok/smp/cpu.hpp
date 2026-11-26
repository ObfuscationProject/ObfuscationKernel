#pragma once

#include "ok/smp/smp.hpp"

#include <array>

namespace ok::smp
{

#if !defined(OK_KERNEL_FREESTANDING) && __STDC_HOSTED__
namespace detail
{
// Hosted model tests stand a thread in for a CPU; nothing here runs on hardware.
inline thread_local CpuId hosted_cpu{0};
} // namespace detail

class ScopedHostedCpu final
{
  public:
    explicit ScopedHostedCpu(CpuId cpu) noexcept : previous_(detail::hosted_cpu)
    {
        detail::hosted_cpu = cpu;
    }
    ~ScopedHostedCpu()
    {
        detail::hosted_cpu = previous_;
    }

    ScopedHostedCpu(const ScopedHostedCpu &) = delete;
    ScopedHostedCpu &operator=(const ScopedHostedCpu &) = delete;

  private:
    CpuId previous_;
};
#endif

[[nodiscard]] inline CpuId current_cpu() noexcept
{
#if defined(OK_KERNEL_FREESTANDING)
    return static_cast<CpuId>(arch::ArchOperations::cpu_index());
#elif __STDC_HOSTED__
    return detail::hosted_cpu;
#else
    // A freestanding unit outside the kernel (a hardware probe's model
    // configuration) has neither the kernel's per-CPU block nor thread-local
    // storage; it runs as CPU 0.
    return 0;
#endif
}

// Identities that stay unique across CPUs without a shared counter (spec
// 4.14): each CPU counts its own and tags the value with its id in the low
// four bits. Only the counting CPU writes its counter; next() gives 0 once
// that CPU's count is exhausted. Values never repeat, and those of one CPU
// increase.
class PerCpuSequence final
{
  public:
    static_assert(max_cpus <= 16, "the CPU tag takes four bits");
    static constexpr u64 limit = ~u64{0} >> 4;

    [[nodiscard]] u64 next() noexcept
    {
        const auto cpu = current_cpu();
        if (cpu >= max_cpus || counts_[cpu] == limit) return 0;
        return (++counts_[cpu] << 4) | cpu;
    }
    [[nodiscard]] bool exhausted() const noexcept
    {
        const auto cpu = current_cpu();
        return cpu >= max_cpus || counts_[cpu] == limit;
    }
    // Fault injection for hosted tests: replaces this CPU's count (`limit`
    // exhausts it) and returns the previous one.
    u64 exchange_count(u64 count) noexcept
    {
        const auto cpu = current_cpu();
        if (cpu >= max_cpus) return 0;
        const auto previous = counts_[cpu];
        counts_[cpu] = count;
        return previous;
    }

  private:
    std::array<u64, max_cpus> counts_{};
};

} // namespace ok::smp
