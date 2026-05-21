#pragma once

#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/smp/smp.hpp"

#include <array>

namespace ok::smp
{

class PreemptionController final
{
  public:
    Status initialize(usize cpu_count);
    void disable(CpuId cpu);
    Status enable(CpuId cpu);
    Status tick(CpuId cpu, sched::Scheduler &scheduler);
    Status sleep_current(CpuId cpu, u64 ticks);
    Status wake_sleepers(u64 now_ticks);
    [[nodiscard]] bool preemptible(CpuId cpu) const;
    [[nodiscard]] u64 ticks(CpuId cpu) const;
    [[nodiscard]] u64 switches(CpuId cpu) const;

  private:
    struct CpuPreemptState
    {
        u32 disable_depth{0};
        u64 ticks{0};
        u64 switches{0};
        u64 sleep_until{0};
    };

    std::array<CpuPreemptState, max_cpus> cpus_{};
    usize cpu_count_{0};
};

} // namespace ok::smp
