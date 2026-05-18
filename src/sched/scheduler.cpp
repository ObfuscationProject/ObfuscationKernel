#include "ok/sched/scheduler.hpp"

namespace ok::sched {

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string_view name) : pid_(pid)
{
    static_cast<void>(name_.assign(name));
}

Result<ProcessId> RoundRobinPolicy::pick_next(std::span<const ProcessControlBlock> processes, ProcessId current)
{
    if (processes.empty()) {
        return Status::not_found("no processes available");
    }

    usize start = 0;
    for (usize i = 0; i < processes.size(); ++i) {
        if (processes[i].pid() == current) {
            start = (i + 1) % processes.size();
            break;
        }
    }

    for (usize offset = 0; offset < processes.size(); ++offset) {
        const auto& process = processes[(start + offset) % processes.size()];
        if (process.state() == ProcessState::runnable || process.state() == ProcessState::running) {
            return process.pid();
        }
    }

    return Status::would_block("no runnable process");
}

Scheduler::Scheduler(SchedulerPolicy& policy) : policy_(&policy)
{
}

SchedulerPolicy& Scheduler::default_round_robin_policy()
{
    static RoundRobinPolicy policy;
    return policy;
}

Result<ProcessId> Scheduler::create_process(std::string_view name, arch::CpuContext initial_context)
{
    if (processes_.full()) {
        return Status::overflow("process table capacity exceeded");
    }
    ProcessControlBlock process {next_pid_++, name};
    if (auto status = process.threads().push_back(ThreadControlBlock {
        .tid = next_tid_++,
        .owner = process.pid(),
        .state = ProcessState::created,
        .context = initial_context,
    });
        !status.ok()) {
        return status;
    }
    const auto pid = process.pid();
    if (auto status = processes_.push_back(process); !status.ok()) {
        return status;
    }
    return pid;
}

Status Scheduler::set_runnable(ProcessId pid)
{
    auto* process = find(pid);
    if (!process) {
        return Status::not_found("process not found");
    }
    process->set_state(ProcessState::runnable);
    for (auto& thread : process->threads()) {
        thread.state = ProcessState::runnable;
    }
    return Status::success();
}

Result<ProcessId> Scheduler::schedule_next()
{
    auto next = policy_->pick_next(std::span<const ProcessControlBlock>(processes_.begin(), processes_.size()), current_pid_);
    if (!next) {
        return next.status();
    }

    if (auto* current = find(current_pid_); current && current->state() == ProcessState::running) {
        current->set_state(ProcessState::runnable);
    }

    auto* selected = find(next.value());
    if (!selected) {
        return Status::not_found("selected process disappeared");
    }
    selected->set_state(ProcessState::running);
    current_pid_ = selected->pid();
    return current_pid_;
}

ProcessControlBlock* Scheduler::find(ProcessId pid)
{
    for (auto& process : processes_) {
        if (process.pid() == pid) {
            return &process;
        }
    }
    return nullptr;
}

const ProcessControlBlock* Scheduler::find(ProcessId pid) const
{
    for (const auto& process : processes_) {
        if (process.pid() == pid) {
            return &process;
        }
    }
    return nullptr;
}

} // namespace ok::sched
