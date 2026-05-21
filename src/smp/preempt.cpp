#include "ok/smp/preempt.hpp"

namespace ok::smp
{

Status PreemptionController::initialize(usize cpu_count)
{
    if (cpu_count == 0 || cpu_count > max_cpus)
    {
        return Status::invalid_argument("invalid preemption CPU count");
    }
    cpu_count_ = cpu_count;
    cpus_ = {};
    return Status::success();
}

void PreemptionController::disable(CpuId cpu)
{
    if (cpu < cpu_count_)
    {
        ++cpus_[cpu].disable_depth;
    }
}

Status PreemptionController::enable(CpuId cpu)
{
    if (cpu >= cpu_count_)
    {
        return Status::invalid_argument("CPU id out of range");
    }
    if (cpus_[cpu].disable_depth == 0)
    {
        return Status::invalid_argument("preemption is not disabled");
    }
    --cpus_[cpu].disable_depth;
    return Status::success();
}

Status PreemptionController::tick(CpuId cpu, sched::Scheduler &scheduler)
{
    if (cpu >= cpu_count_)
    {
        return Status::invalid_argument("CPU id out of range");
    }
    ++cpus_[cpu].ticks;
    if (!preemptible(cpu))
    {
        return Status::success();
    }
    auto next = scheduler.schedule_next_on_cpu(cpu);
    if (!next)
    {
        return next.status();
    }
    ++cpus_[cpu].switches;
    return Status::success();
}

Status PreemptionController::sleep_current(CpuId cpu, u64 ticks)
{
    if (cpu >= cpu_count_)
    {
        return Status::invalid_argument("CPU id out of range");
    }
    cpus_[cpu].sleep_until = cpus_[cpu].ticks + ticks;
    return Status::success();
}

Status PreemptionController::wake_sleepers(u64 now_ticks)
{
    for (usize i = 0; i < cpu_count_; ++i)
    {
        if (cpus_[i].sleep_until != 0 && cpus_[i].sleep_until <= now_ticks)
        {
            cpus_[i].sleep_until = 0;
        }
    }
    return Status::success();
}

bool PreemptionController::preemptible(CpuId cpu) const
{
    return cpu < cpu_count_ && cpus_[cpu].disable_depth == 0 && cpus_[cpu].sleep_until == 0;
}

u64 PreemptionController::ticks(CpuId cpu) const
{
    return cpu < cpu_count_ ? cpus_[cpu].ticks : 0;
}

u64 PreemptionController::switches(CpuId cpu) const
{
    return cpu < cpu_count_ ? cpus_[cpu].switches : 0;
}

} // namespace ok::smp
