#include "ok/sched/scheduler.hpp"

namespace ok::sched
{
namespace
{

bool process_has_runnable_thread(const ProcessControlBlock &process)
{
    for (const auto &thread : process.threads())
    {
        if (thread.state == ProcessState::runnable)
        {
            return true;
        }
    }
    return false;
}

bool process_allowed_on(const ProcessControlBlock &process, smp::CpuId cpu, ProcessId current)
{
    if (!process.can_run_on(cpu))
    {
        return false;
    }
    if (process.state() == ProcessState::runnable)
    {
        return true;
    }
    if (process.state() == ProcessState::running)
    {
        return process.pid() == current || process_has_runnable_thread(process);
    }
    return false;
}

bool is_idle_process(const ProcessControlBlock &process)
{
    return process.name() == "idle";
}

} // namespace

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string_view name, bool background)
    : pid_(pid), background_(background)
{
    static_cast<void>(name_.assign(name));
}

Result<ProcessId> RoundRobinPolicy::pick_next(std::span<const ProcessControlBlock> processes, ProcessId current,
                                              smp::CpuId cpu)
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

    u8 best_priority = scheduler_min_priority;
    bool found = false;
    for (usize offset = 0; offset < processes.size(); ++offset)
    {
        const auto &process = processes[(start + offset) % processes.size()];
        if (!process_allowed_on(process, cpu, current))
        {
            continue;
        }
        if (!found || process.priority() > best_priority)
        {
            best_priority = process.priority();
            found = true;
        }
    }

    if (!found)
    {
        return Status::would_block("no runnable process");
    }

    for (usize offset = 0; offset < processes.size(); ++offset)
    {
        const auto &process = processes[(start + offset) % processes.size()];
        if (process_allowed_on(process, cpu, current) && process.priority() == best_priority)
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
    FixedString<max_process_name> validated_name;
    if (auto status = validated_name.assign(name); !status.ok())
    {
        return status;
    }
    ProcessControlBlock process{next_pid_++, validated_name.view()};
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

Result<ThreadId> Scheduler::create_thread(ProcessId pid, arch::CpuContext initial_context)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    if (process->state() == ProcessState::exited)
    {
        return Status::invalid_argument("cannot create a thread for an exited process");
    }
    const auto tid = next_tid_++;
    if (auto status = process->threads().push_back(ThreadControlBlock{
            .tid = tid,
            .owner = pid,
            .state = ProcessState::runnable,
            .context = initial_context,
            .priority = process->priority(),
        });
        !status.ok())
    {
        return status;
    }
    if (process->state() == ProcessState::created || process->state() == ProcessState::blocked)
    {
        process->set_state(ProcessState::runnable);
    }
    return tid;
}

Result<ProcessId> Scheduler::spawn(ScheduleRequest request)
{
    if (request.name.empty())
    {
        return Status::invalid_argument("scheduled process name is empty");
    }
    if (request.priority > scheduler_max_priority)
    {
        return Status::invalid_argument("scheduled process priority is out of range");
    }
    if ((request.cpu_affinity_mask & configured_cpu_mask()) == 0)
    {
        return Status::invalid_argument("scheduled process CPU affinity has no configured CPU");
    }

    auto process = request.background ? create_background_process(request.name, request.initial_context)
                                      : create_process(request.name, request.initial_context);
    if (!process)
    {
        return process.status();
    }
    if (!request.background)
    {
        if (auto status = set_runnable(process.value()); !status.ok())
        {
            static_cast<void>(kill_process(process.value()));
            return status;
        }
    }
    if (auto status = set_credentials(process.value(), request.credentials); !status.ok())
    {
        static_cast<void>(kill_process(process.value()));
        return status;
    }
    if (auto status = set_priority(process.value(), request.priority); !status.ok())
    {
        static_cast<void>(kill_process(process.value()));
        return status;
    }
    if (auto status = set_cpu_affinity(process.value(), request.cpu_affinity_mask); !status.ok())
    {
        static_cast<void>(kill_process(process.value()));
        return status;
    }
    return process.value();
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

Status Scheduler::set_priority(ProcessId pid, u8 priority)
{
    if (priority > scheduler_max_priority)
    {
        return Status::invalid_argument("process priority is out of range");
    }
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_priority(priority);
    for (auto &thread : process->threads())
    {
        thread.priority = priority;
    }
    return Status::success();
}

Status Scheduler::set_cpu_affinity(ProcessId pid, u16 cpu_affinity_mask)
{
    const auto valid_mask = configured_cpu_mask();
    const auto effective_mask = static_cast<u16>(cpu_affinity_mask & valid_mask);
    if (effective_mask == 0)
    {
        return Status::invalid_argument("CPU affinity mask has no configured CPU");
    }
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_cpu_affinity_mask(effective_mask);
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
        for (usize cpu = 0; cpu < current_by_cpu_.size(); ++cpu)
        {
            if (current_by_cpu_[cpu] == pid)
            {
                current_by_cpu_[cpu] = 0;
                current_thread_by_cpu_[cpu] = 0;
            }
        }
        for (auto &stats : cpu_stats_)
        {
            if (stats.current_pid == pid)
            {
                stats.current_pid = 0;
                stats.current_tid = 0;
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
    for (auto &tid : current_thread_by_cpu_)
    {
        tid = 0;
    }
    for (auto &stats : cpu_stats_)
    {
        stats = {};
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

Status Scheduler::block_process(ProcessId pid)
{
    auto *process = find(pid);
    if (!process)
    {
        return Status::not_found("process not found");
    }
    process->set_state(ProcessState::blocked);
    for (auto &thread : process->threads())
    {
        if (thread.state != ProcessState::exited)
        {
            thread.state = ProcessState::blocked;
        }
    }
    for (usize i = 0; i < current_by_cpu_.size(); ++i)
    {
        if (current_by_cpu_[i] == pid)
        {
            current_by_cpu_[i] = 0;
            current_thread_by_cpu_[i] = 0;
            cpu_stats_[i].current_pid = 0;
            cpu_stats_[i].current_tid = 0;
        }
    }
    if (current_pid_ == pid)
    {
        current_pid_ = 0;
    }
    return Status::success();
}

Result<ProcessId> Scheduler::schedule_next()
{
    return schedule_next_on_cpu(0);
}

bool Scheduler::thread_running_on_other_cpu(ThreadId tid, smp::CpuId cpu) const
{
    if (tid == 0)
    {
        return false;
    }
    for (usize i = 0; i < cpu_count_; ++i)
    {
        if (i != cpu && current_thread_by_cpu_[i] == tid)
        {
            return true;
        }
    }
    return false;
}

Result<ThreadId> Scheduler::pick_next_thread(ProcessControlBlock &process, ThreadId current, smp::CpuId cpu)
{
    auto &threads = process.threads();
    if (threads.empty())
    {
        return Status::not_found("process has no threads");
    }

    usize start = 0;
    for (usize i = 0; i < threads.size(); ++i)
    {
        if (threads[i].tid == current)
        {
            start = (i + 1) % threads.size();
            break;
        }
    }

    for (usize offset = 0; offset < threads.size(); ++offset)
    {
        auto &thread = threads[(start + offset) % threads.size()];
        if (thread.state == ProcessState::runnable ||
            (thread.state == ProcessState::running && thread.tid == current && !thread_running_on_other_cpu(thread.tid, cpu)))
        {
            return thread.tid;
        }
    }

    return Status::would_block("process has no runnable thread");
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
    const auto previous_tid = current_thread_by_cpu_[cpu];
    auto next =
        policy_->pick_next(std::span<const ProcessControlBlock>(processes_.begin(), processes_.size()), previous_pid,
                           cpu);
    if (!next)
    {
        return next.status();
    }

    if (auto *current = find(previous_pid); current && current->state() == ProcessState::running)
    {
        for (auto &thread : current->threads())
        {
            if (thread.tid == previous_tid && thread.state == ProcessState::running)
            {
                thread.state = ProcessState::runnable;
                break;
            }
        }
        current->set_state(ProcessState::runnable);
    }

    auto *selected = find(next.value());
    if (!selected)
    {
        return Status::not_found("selected process disappeared");
    }
    auto selected_thread = pick_next_thread(*selected, selected->pid() == previous_pid ? previous_tid : 0, cpu);
    if (!selected_thread)
    {
        return selected_thread.status();
    }
    for (auto &thread : selected->threads())
    {
        if (thread.tid == selected_thread.value())
        {
            thread.state = ProcessState::running;
            ++thread.dispatch_count;
            ++thread.cpu_time_ticks;
            thread.last_cpu = cpu;
            break;
        }
    }
    selected->set_state(ProcessState::running);
    selected->record_dispatch(cpu);
    current_pid_ = selected->pid();
    current_by_cpu_[cpu] = selected->pid();
    current_thread_by_cpu_[cpu] = selected_thread.value();
    ++cpu_stats_[cpu].dispatches;
    if (!is_idle_process(*selected))
    {
        ++cpu_stats_[cpu].busy_dispatches;
    }
    cpu_stats_[cpu].current_pid = selected->pid();
    cpu_stats_[cpu].current_tid = selected_thread.value();
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

ThreadId Scheduler::current_tid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return current_thread_by_cpu_[cpu];
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

CpuSchedulingStats Scheduler::cpu_stats(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return {};
    }
    return cpu_stats_[cpu];
}

u64 Scheduler::total_dispatches() const
{
    u64 total = 0;
    for (usize i = 0; i < cpu_count_; ++i)
    {
        total += cpu_stats_[i].dispatches;
    }
    return total;
}

u8 Scheduler::cpu_usage_percent(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_ || cpu_stats_[cpu].dispatches == 0)
    {
        return 0;
    }
    return static_cast<u8>((cpu_stats_[cpu].busy_dispatches * 100u) / cpu_stats_[cpu].dispatches);
}

u8 Scheduler::process_usage_percent(ProcessId pid) const
{
    const auto total = total_dispatches();
    if (total == 0)
    {
        return 0;
    }
    const auto *process = find(pid);
    if (process == nullptr)
    {
        return 0;
    }
    return static_cast<u8>((process->dispatch_count() * 100u) / total);
}

u16 Scheduler::configured_cpu_mask() const
{
    if (cpu_count_ >= smp::max_cpus)
    {
        return cpu_affinity_any;
    }
    return static_cast<u16>((1u << cpu_count_) - 1u);
}

} // namespace ok::sched
