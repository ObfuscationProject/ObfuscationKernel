#include "ok/sched/scheduler.hpp"

#include <algorithm>

namespace ok::sched {

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string name)
    : pid_(pid), name_(std::move(name))
{
}

Result<ProcessId> RoundRobinPolicy::pick_next(std::span<const std::shared_ptr<ProcessControlBlock>> processes,
                                              ProcessId current)
{
    if (processes.empty()) {
        return Status::not_found("no processes available");
    }

    usize start = 0;
    for (usize i = 0; i < processes.size(); ++i) {
        if (processes[i]->pid() == current) {
            start = (i + 1) % processes.size();
            break;
        }
    }

    for (usize offset = 0; offset < processes.size(); ++offset) {
        const auto& process = processes[(start + offset) % processes.size()];
        if (process->state() == ProcessState::runnable || process->state() == ProcessState::running) {
            return process->pid();
        }
    }

    return Status::would_block("no runnable process");
}

Scheduler::Scheduler(std::unique_ptr<SchedulerPolicy> policy)
    : policy_(std::move(policy))
{
}

Result<ProcessId> Scheduler::create_process(std::string name, arch::CpuContext initial_context)
{
    auto process = std::make_shared<ProcessControlBlock>(next_pid_++, std::move(name));
    process->threads().push_back(ThreadControlBlock {
        .tid = next_tid_++,
        .owner = process->pid(),
        .state = ProcessState::created,
        .context = initial_context,
    });
    const auto pid = process->pid();
    processes_.push_back(std::move(process));
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
    auto next = policy_->pick_next(std::span<const std::shared_ptr<ProcessControlBlock>>(processes_), current_pid_);
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
    const auto it = std::find_if(processes_.begin(), processes_.end(), [pid](const auto& process) {
        return process->pid() == pid;
    });
    return it == processes_.end() ? nullptr : it->get();
}

const ProcessControlBlock* Scheduler::find(ProcessId pid) const
{
    const auto it = std::find_if(processes_.begin(), processes_.end(), [pid](const auto& process) {
        return process->pid() == pid;
    });
    return it == processes_.end() ? nullptr : it->get();
}

} // namespace ok::sched

