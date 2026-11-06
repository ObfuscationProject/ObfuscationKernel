#pragma once

#include "ok/smp/smp.hpp"

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

} // namespace ok::smp
