#include "ok/smp/smp.hpp"

namespace ok::smp {

Status CpuTopology::initialize(usize cpu_count)
{
    if (cpu_count == 0) {
        return Status::invalid_argument("cpu count must be non-zero");
    }
    if (cpu_count > max_cpus) {
        return Status::overflow("cpu topology capacity exceeded");
    }

    cpu_count_ = cpu_count;
    for (usize i = 0; i < cpus_.size(); ++i) {
        cpus_[i] = CpuInfo {.id = static_cast<CpuId>(i), .state = CpuState::offline};
    }
    cpus_[0].state = CpuState::boot;
    return Status::success();
}

Status CpuTopology::mark_starting(CpuId cpu)
{
    if (!valid(cpu)) {
        return Status::invalid_argument("cpu id out of range");
    }
    cpus_[cpu].state = CpuState::starting;
    return Status::success();
}

Status CpuTopology::mark_online(CpuId cpu)
{
    if (!valid(cpu)) {
        return Status::invalid_argument("cpu id out of range");
    }
    cpus_[cpu].state = CpuState::online;
    return Status::success();
}

Status CpuTopology::mark_halted(CpuId cpu)
{
    if (!valid(cpu)) {
        return Status::invalid_argument("cpu id out of range");
    }
    cpus_[cpu].state = CpuState::halted;
    return Status::success();
}

Status CpuTopology::record_schedule(CpuId cpu)
{
    if (!valid(cpu)) {
        return Status::invalid_argument("cpu id out of range");
    }
    ++cpus_[cpu].scheduler_ticks;
    return Status::success();
}

usize CpuTopology::online_count() const
{
    usize count = 0;
    for (usize i = 0; i < cpu_count_; ++i) {
        if (cpus_[i].state == CpuState::boot || cpus_[i].state == CpuState::online) {
            ++count;
        }
    }
    return count;
}

Result<CpuId> CpuTopology::least_busy_online_cpu() const
{
    const CpuInfo* selected = nullptr;
    for (usize i = 0; i < cpu_count_; ++i) {
        const auto& cpu_info = cpus_[i];
        if (cpu_info.state != CpuState::boot && cpu_info.state != CpuState::online) {
            continue;
        }
        if (selected == nullptr || cpu_info.scheduler_ticks < selected->scheduler_ticks) {
            selected = &cpu_info;
        }
    }
    if (selected == nullptr) {
        return Status::would_block("no online cpu available");
    }
    return selected->id;
}

const CpuInfo* CpuTopology::cpu(CpuId cpu) const
{
    if (!valid(cpu)) {
        return nullptr;
    }
    return &cpus_[cpu];
}

} // namespace ok::smp
