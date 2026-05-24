#include "ok/sched/scheduler.hpp"

namespace ok::sched
{

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string_view name, bool background)
    : pid_(pid), background_(background)
{
    static_cast<void>(name_.assign(name));
}

Result<ProcessId> RoundRobinPolicy::pick_next(std::span<const ProcessControlBlock> processes, ProcessId current)
{
    if (processes.empty())
    {
        return Status::not_found("no processes available");
    }

    usize start = 0;
    for (usize i = 0; i < processes.size(); ++i)
    {
        if (processes[i].pid() == current)
        {
            start = (i + 1) % processes.size();
            break;
        }
    }

    for (usize offset = 0; offset < processes.size(); ++offset)
    {
        const auto &process = processes[(start + offset) % processes.size()];
        if (process.state() == ProcessState::runnable || process.state() == ProcessState::running)
        {
            return process.pid();
        }
    }

    return Status::would_block("no runnable process");
}

Scheduler::Scheduler(SchedulerPolicy *policy) : policy_(policy == nullptr ? &owned_round_robin_policy_ : policy)
{
}

SchedulerPolicy &Scheduler::default_round_robin_policy()
{
    static RoundRobinPolicy policy;
    return policy;
}

Result<ProcessId> Scheduler::create_process(std::string_view name, arch::CpuContext initial_context)
{
    if (processes_.full())
    {
        return Status::overflow("process table capacity exceeded");
    }
    ProcessControlBlock process{next_pid_++, name};
    if (auto status = process.threads().push_back(ThreadControlBlock{
            .tid = next_tid_++,
            .owner = process.pid(),
            .state = ProcessState::created,
            .context = initial_context,
        });
        !status.ok())
    {
        return status;
    }
    const auto pid = process.pid();
    if (auto status = processes_.push_back(process); !status.ok())
    {
        return status;
    }
    return pid;
}

Result<ProcessId> Scheduler::create_user_process(std::string_view name, arch::CpuContext initial_context)
{
    auto pid = create_process(name, initial_context);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = find(pid.value());
    if (process == nullptr)
    {
        return Status::fault("created user process is not visible");
    }
    process->set_execution(ProcessExecution::user_process);
    process->set_address_space_id(next_address_space_id_++);
    return pid.value();
}

Result<ProcessId> Scheduler::create_background_process(std::string_view name, arch::CpuContext initial_context)
{
    auto pid = create_process(name, initial_context);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = find(pid.value());
    if (process == nullptr)
    {
        return Status::fault("background process is not visible");
    }
    process->set_background(true);
    if (auto status = set_runnable(pid.value()); !status.ok())
    {
        return status;
    }
    return pid.value();
}

Status Scheduler::set_credentials(ProcessId pid, user::Credentials credentials)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_credentials(credentials);
    return Status::success();
}

Status Scheduler::kill_process(ProcessId pid)
{
    for (usize i = 0; i < processes_.size(); ++i)
    {
        if (processes_[i].pid() != pid)
        {
            continue;
        }
        processes_[i].set_state(ProcessState::exited);
        for (auto &thread : processes_[i].threads())
        {
            thread.state = ProcessState::exited;
        }
        if (current_pid_ == pid)
        {
            current_pid_ = 0;
        }
        for (auto &current : current_by_cpu_)
        {
            if (current == pid)
            {
                current = 0;
            }
        }
        return processes_.erase_at(i);
    }
    return Status::not_found("process not found");
}

Status Scheduler::configure_cpus(usize cpu_count)
{
    if (cpu_count == 0)
    {
        return Status::invalid_argument("scheduler cpu count must be non-zero");
    }
    if (cpu_count > smp::max_cpus)
    {
        return Status::overflow("scheduler cpu capacity exceeded");
    }
    cpu_count_ = cpu_count;
    for (auto &pid : current_by_cpu_)
    {
        pid = 0;
    }
    current_pid_ = 0;
    return Status::success();
}

Status Scheduler::set_runnable(ProcessId pid)
{
    auto *process = find(pid);
    if (!process)
    {
        return Status::not_found("process not found");
    }
    process->set_state(ProcessState::runnable);
    for (auto &thread : process->threads())
    {
        thread.state = ProcessState::runnable;
    }
    return Status::success();
}

Result<ProcessId> Scheduler::schedule_next()
{
    return schedule_next_on_cpu(0);
}

Result<ProcessId> Scheduler::schedule_next_on_cpu(smp::CpuId cpu)
{
    if (policy_ == nullptr)
    {
        policy_ = &owned_round_robin_policy_;
    }
    if (cpu >= cpu_count_)
    {
        return Status::invalid_argument("scheduler cpu id out of range");
    }

    const auto previous_pid = current_by_cpu_[cpu];
    auto next =
        policy_->pick_next(std::span<const ProcessControlBlock>(processes_.begin(), processes_.size()), previous_pid);
    if (!next)
    {
        return next.status();
    }

    if (auto *current = find(previous_pid); current && current->state() == ProcessState::running)
    {
        current->set_state(ProcessState::runnable);
    }

    auto *selected = find(next.value());
    if (!selected)
    {
        return Status::not_found("selected process disappeared");
    }
    selected->set_state(ProcessState::running);
    current_pid_ = selected->pid();
    current_by_cpu_[cpu] = selected->pid();
    return current_pid_;
}

ProcessId Scheduler::current_pid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return current_by_cpu_[cpu];
}

ProcessControlBlock *Scheduler::find(ProcessId pid)
{
    for (auto &process : processes_)
    {
        if (process.pid() == pid)
        {
            return &process;
        }
    }
    return nullptr;
}

const ProcessControlBlock *Scheduler::find(ProcessId pid) const
{
    for (const auto &process : processes_)
    {
        if (process.pid() == pid)
        {
            return &process;
        }
    }
    return nullptr;
}

usize Scheduler::background_process_count() const
{
    usize count = 0;
    for (const auto &process : processes_)
    {
        if (process.background())
        {
            ++count;
        }
    }
    return count;
}

} // namespace ok::sched
