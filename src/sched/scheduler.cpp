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

bool process_counts_as_cpu_work(const ProcessControlBlock &process)
{
    return process.cpu_accounting() == ProcessCpuAccounting::accounted && !is_idle_process(process);
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

Result<ProcessId> SchedulerState::create_process(std::string_view name, arch::CpuContext initial_context)
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
    initialize_resources(process.resources());
    const auto pid = process.pid();
    if (auto status = processes_.push_back(process); !status.ok())
    {
        return status;
    }
    return pid;
}

Result<ProcessId> SchedulerState::create_user_process(std::string_view name, arch::CpuContext initial_context)
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

Result<ProcessId> SchedulerState::create_background_process(std::string_view name, arch::CpuContext initial_context)
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

Result<ThreadId> SchedulerState::create_thread(ProcessId pid, arch::CpuContext initial_context)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    if (process->state() == ProcessState::exited || process->state() == ProcessState::zombie)
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

Result<ProcessId> SchedulerState::spawn(ScheduleRequest request)
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
    if (auto status = set_cpu_accounting(process.value(), request.cpu_accounting); !status.ok())
    {
        static_cast<void>(kill_process(process.value()));
        return status;
    }
    return process.value();
}

Status SchedulerState::set_credentials(ProcessId pid, user::Credentials credentials)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_credentials(credentials);
    return Status::success();
}

Status SchedulerState::set_priority(ProcessId pid, u8 priority)
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

Status SchedulerState::set_cpu_affinity(ProcessId pid, u16 cpu_affinity_mask)
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

Status SchedulerState::set_cpu_accounting(ProcessId pid, ProcessCpuAccounting accounting)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_cpu_accounting(accounting);
    return Status::success();
}

Status SchedulerState::kill_process(ProcessId pid)
{
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        if (auto status = exit_process(pid, -1); !status.ok())
            return status;
    return reap_process(pid);
}

Status SchedulerState::configure_cpus(usize cpu_count)
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

Status SchedulerState::set_runnable(ProcessId pid)
{
    auto *process = find(pid);
    if (!process)
    {
        return Status::not_found("process not found");
    }
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::invalid_argument("cannot run an exited process");
    process->set_state(ProcessState::runnable);
    for (auto &thread : process->threads())
    {
        thread.state = ProcessState::runnable;
    }
    return Status::success();
}

Status SchedulerState::block_process(ProcessId pid)
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

bool SchedulerState::thread_running_on_other_cpu(ThreadId tid, smp::CpuId cpu) const
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

Result<ThreadId> SchedulerState::pick_next_thread(ProcessControlBlock &process, ThreadId current, smp::CpuId cpu)
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
        if (thread.state == ProcessState::runnable || (thread.state == ProcessState::running && thread.tid == current &&
                                                       !thread_running_on_other_cpu(thread.tid, cpu)))
        {
            return thread.tid;
        }
    }

    return Status::would_block("process has no runnable thread");
}

Result<ProcessId> SchedulerState::dispatch_selected(smp::CpuId cpu, Result<ProcessId> next)
{
    if (cpu >= cpu_count_)
    {
        return Status::invalid_argument("scheduler cpu id out of range");
    }
    const auto previous_pid = current_by_cpu_[cpu];
    const auto previous_tid = current_thread_by_cpu_[cpu];
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
    const bool account_cpu = process_counts_as_cpu_work(*selected);
    selected->record_dispatch(cpu, account_cpu);
    current_pid_ = selected->pid();
    current_by_cpu_[cpu] = selected->pid();
    current_thread_by_cpu_[cpu] = selected_thread.value();
    ++cpu_stats_[cpu].dispatches;
    if (account_cpu)
    {
        ++cpu_stats_[cpu].busy_dispatches;
    }
    cpu_stats_[cpu].current_pid = selected->pid();
    cpu_stats_[cpu].current_tid = selected_thread.value();
    return current_pid_;
}

ProcessId SchedulerState::current_pid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return current_by_cpu_[cpu];
}

ThreadId SchedulerState::current_tid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return current_thread_by_cpu_[cpu];
}

ProcessControlBlock *SchedulerState::find(ProcessId pid)
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

const ProcessControlBlock *SchedulerState::find(ProcessId pid) const
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

usize SchedulerState::background_process_count() const
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

CpuSchedulingStats SchedulerState::cpu_stats(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return {};
    }
    return cpu_stats_[cpu];
}

u64 SchedulerState::total_dispatches() const
{
    u64 total = 0;
    for (usize i = 0; i < cpu_count_; ++i)
    {
        total += cpu_stats_[i].dispatches;
    }
    return total;
}

u64 SchedulerState::total_accounted_dispatches() const
{
    u64 total = 0;
    for (usize i = 0; i < cpu_count_; ++i)
    {
        total += cpu_stats_[i].busy_dispatches;
    }
    return total;
}

u8 SchedulerState::cpu_usage_percent(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_ || cpu_stats_[cpu].dispatches == 0)
    {
        return 0;
    }
    return static_cast<u8>((cpu_stats_[cpu].busy_dispatches * 100u) / cpu_stats_[cpu].dispatches);
}

u8 SchedulerState::process_usage_percent(ProcessId pid) const
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
    return static_cast<u8>((process->accounted_cpu_time_ticks() * 100u) / total);
}

u16 SchedulerState::configured_cpu_mask() const
{
    if (cpu_count_ >= smp::max_cpus)
    {
        return cpu_affinity_any;
    }
    return static_cast<u16>((1u << cpu_count_) - 1u);
}

SchedulerState::SchedulerState()
{
    descriptions_[0] =
        OpenFileDescription{.used = true, .readable = true, .input_console = true, .path = FixedString<96>{"stdin"}};
    descriptions_[1] =
        OpenFileDescription{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stdout"}};
    descriptions_[2] =
        OpenFileDescription{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stderr"}};
    initialize_resources(boot_resources_);
}

void SchedulerState::initialize_resources(ProcessResources &resources)
{
    resources = {};
    for (i32 fd = 0; fd < 3; ++fd)
        static_cast<void>(install_descriptor(resources, fd, static_cast<u16>(fd)));
}

OpenFileDescription *SchedulerState::open_description(u16 id)
{
    return id < descriptions_.size() && descriptions_[id].used ? &descriptions_[id] : nullptr;
}
const OpenFileDescription *SchedulerState::open_description(u16 id) const
{
    return id < descriptions_.size() && descriptions_[id].used ? &descriptions_[id] : nullptr;
}
Result<u16> SchedulerState::allocate_description(OpenFileDescription description)
{
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
        {
            description.used = true;
            description.references = 0;
            descriptions_[i] = description;
            return static_cast<u16>(i);
        }
    return Status::overflow("open file description table full");
}
Status SchedulerState::install_descriptor(ProcessResources &resources, i32 fd, u16 description, bool close_on_exec)
{
    auto *slot = resources.files.entry(fd);
    auto *open = open_description(description);
    if (slot == nullptr || open == nullptr)
        return Status::invalid_argument("invalid descriptor binding");
    // Retain first: dup2 may replace the last other reference to this description.
    ++open->references;
    if (slot->used)
        static_cast<void>(close_descriptor(resources, fd));
    *slot = FileDescriptor{.used = true, .close_on_exec = close_on_exec, .description = description};
    return Status::success();
}
Status SchedulerState::close_descriptor(ProcessResources &resources, i32 fd)
{
    auto *slot = resources.files.entry(fd);
    if (slot == nullptr || !slot->used)
        return Status::invalid_argument("bad file descriptor");
    if (auto *open = open_description(slot->description); open != nullptr && open->references != 0)
    {
        --open->references;
        const auto pipe = open->pipe;
        if (open->references == 0 && slot->description >= 3)
            *open = {};
        if (pipe < pipes_.size())
        {
            wake_waiters(ThreadWaitReason::pipe_read, pipe);
            wake_waiters(ThreadWaitReason::pipe_write, pipe);
            if (!pipe_has_endpoint(pipe, false) && !pipe_has_endpoint(pipe, true))
                pipes_[pipe] = {};
        }
    }
    *slot = {};
    return Status::success();
}
void SchedulerState::release_resources(ProcessResources &resources)
{
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds); ++fd)
        if (resources.files.contains(fd))
            static_cast<void>(close_descriptor(resources, fd));
    resources.memory = {};
    resources.mappings = {};
}
usize SchedulerState::open_description_count() const
{
    usize count = 0;
    for (const auto &open : descriptions_)
        if (open.used && open.references != 0)
            ++count;
    return count;
}
void SchedulerState::clear_current(ProcessId pid)
{
    if (current_pid_ == pid)
        current_pid_ = 0;
    for (usize cpu = 0; cpu < current_by_cpu_.size(); ++cpu)
        if (current_by_cpu_[cpu] == pid)
        {
            current_by_cpu_[cpu] = 0;
            current_thread_by_cpu_[cpu] = 0;
            cpu_stats_[cpu].current_pid = 0;
            cpu_stats_[cpu].current_tid = 0;
        }
}
Status SchedulerState::activate(ProcessId pid, smp::CpuId cpu)
{
    if (cpu >= cpu_count_)
        return Status::invalid_argument("invalid activation CPU");
    if (pid == 0)
    {
        if (auto *previous = find(current_by_cpu_[cpu]); previous != nullptr)
        {
            for (auto &thread : previous->threads())
                if (thread.tid == current_thread_by_cpu_[cpu] && thread.state == ProcessState::running)
                    thread.state = ProcessState::runnable;
            if (previous->state() == ProcessState::running)
                previous->set_state(ProcessState::runnable);
        }
        current_pid_ = 0;
        current_by_cpu_[cpu] = 0;
        current_thread_by_cpu_[cpu] = 0;
        cpu_stats_[cpu].current_pid = 0;
        cpu_stats_[cpu].current_tid = 0;
        return Status::success();
    }
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() == ProcessState::blocked || process->state() == ProcessState::zombie ||
        process->state() == ProcessState::exited || !process->can_run_on(cpu))
        return Status::invalid_argument("process cannot be activated");
    if (process->state() == ProcessState::created)
        static_cast<void>(set_runnable(pid));
    auto selected = dispatch_selected(cpu, pid);
    return selected ? Status::success() : selected.status();
}
Result<ProcessId> SchedulerState::fork_process(ProcessId parent)
{
    auto *source = find(parent);
    if (source == nullptr)
        return Status::not_found("parent process not found");
    if (source->state() == ProcessState::zombie || source->state() == ProcessState::exited || source->threads().empty())
        return Status::invalid_argument("parent process is not alive");
    if (source->children().full())
        return Status::overflow("child table full");
    const ThreadControlBlock *calling_thread = &source->threads()[0];
    for (const auto &thread : source->threads())
        if (thread.tid == current_tid(0) && current_pid(0) == parent)
            calling_thread = &thread;
    auto child_pid = create_process(source->name(), calling_thread->context);
    if (!child_pid)
        return child_pid.status();
    auto *child = find(child_pid.value());
    release_resources(child->resources());
    child->resources() = source->resources();
    // Native runtime materializes a distinct address space before publishing the child.
    child->resources().native_space = {};
    child->resources().native_pages = {};
    for (const auto &slot : child->fd_table().entries())
        if (slot.used)
            if (auto *description = open_description(slot.description))
                ++description->references;
    child->set_parent(parent);
    child->set_credentials(source->credentials());
    child->set_execution(source->execution());
    child->set_address_space_id(next_address_space_id_++);
    child->set_background(source->background());
    child->set_priority(source->priority());
    child->set_cpu_affinity_mask(source->cpu_affinity_mask());
    child->threads()[0].priority = calling_thread->priority;
    child->threads()[0].tls = calling_thread->tls;
    child->threads()[0].native_frame = calling_thread->native_frame;
    child->threads()[0].native_active = false;
    child->threads()[0].syscall_pending = false;
    child->threads()[0].tls.clear_tid_address = 0;
    child->threads()[0].wait_reason = ThreadWaitReason::none;
    static_cast<void>(source->add_child(child_pid.value()));
    static_cast<void>(set_runnable(child_pid.value()));
    return child_pid;
}
Status SchedulerState::replace_image(ProcessId pid, arch::CpuContext context, const MemoryMap &memory)
{
    if (auto status = arch::validate_architecture(context.architecture); !status.ok())
        return status;
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::invalid_argument("cannot exec an exited process");
    if (process->threads().empty())
        return Status::invalid_argument("process has no thread");
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds); ++fd)
    {
        const auto *slot = process->fd_table().entry(fd);
        if (slot->used && slot->close_on_exec)
            static_cast<void>(close_descriptor(process->resources(), fd));
    }
    process->memory_map() = memory;
    process->resources().mappings = {};
    process->resources().program_break = process->resources().program_break_base;
    process->resources().next_mapping_address = 0x2000'0000;
    process->signals() = {};
    while (process->threads().size() > 1)
        static_cast<void>(process->threads().erase_at(1));
    auto &thread = process->threads()[0];
    thread.context = context;
    thread.tls = {};
    thread.wait_reason = ThreadWaitReason::none;
    thread.wait_key = 0;
    thread.state = ProcessState::runnable;
    process->set_state(ProcessState::runnable);
    process->set_execution(context.mode == arch::PrivilegeMode::user ? ProcessExecution::user_process
                                                                     : ProcessExecution::kernel_thread);
    process->set_address_space_id(next_address_space_id_++);
    clear_current(pid);
    return Status::success();
}
Status SchedulerState::exit_process(ProcessId pid, i32 code)
{
    return finish_process(pid, code, 0);
}
Status SchedulerState::terminate_process(ProcessId pid, u8 signal)
{
    if (signal == 0 || signal > 127)
        return Status::invalid_argument("invalid termination signal");
    return finish_process(pid, 0, signal);
}
Status SchedulerState::finish_process(ProcessId pid, i32 code, u8 signal)
{
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::already_exists("process has already exited");
    process->set_exit_code(code);
    process->set_termination_signal(signal);
    process->set_state(ProcessState::zombie);
    for (auto &thread : process->threads())
        thread.state = ProcessState::exited;
    release_resources(process->resources());
    // Parent 0 is owned by the service reaper, not a waitable process.
    static_cast<void>(reparent_children(pid, 0));
    clear_current(pid);
    wake_waiters(ThreadWaitReason::child, pid);
    wake_waiters(ThreadWaitReason::child, 0);
    return Status::success();
}
Result<i32> SchedulerState::wait_process(ProcessId parent, ProcessId child)
{
    auto *parent_process = find(parent);
    auto *process = find(child);
    if (parent_process == nullptr || process == nullptr || process->parent_pid() != parent)
        return Status::not_found("wait child not found");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
    {
        if (current_pid() == parent)
            static_cast<void>(block_current(ThreadWaitReason::child, child));
        return Status::would_block("child has not exited");
    }
    const auto code = process->exit_code();
    if (auto status = reap_process(child); !status.ok())
        return status;
    return code;
}
Status SchedulerState::reparent_children(ProcessId from, ProcessId to)
{
    if (from == to)
        return Status::invalid_argument("cannot reparent to the same process");
    auto *target = find(to);
    auto *source = find(from);
    if (source == nullptr || (to != 0 && target == nullptr))
        return Status::not_found("reparent process not found");
    if (target != nullptr && target->children().size() + source->children().size() > max_process_children)
        return Status::overflow("reparent child table full");
    for (const auto child : source->children())
    {
        auto *process = find(child);
        if (process == nullptr)
            continue;
        process->set_parent(to);
        if (target != nullptr)
            static_cast<void>(target->add_child(child));
    }
    source->children().clear();
    return Status::success();
}
Status SchedulerState::reap_process(ProcessId pid)
{
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        return Status::invalid_argument("process is not a zombie");
    if (process->resources().native_space.root != 0 || !process->resources().native_pages.empty())
        return Status::busy("native address space must be released before reaping");
    const auto parent = process->parent_pid();
    if (auto *owner = find(parent))
        static_cast<void>(owner->remove_child(pid));
    static_cast<void>(reparent_children(pid, 0));
    release_resources(process->resources());
    clear_current(pid);
    for (usize i = 0; i < processes_.size(); ++i)
        if (processes_[i].pid() == pid)
            return processes_.erase_at(i);
    return Status::not_found("process not found");
}

usize SchedulerState::reap_orphans()
{
    usize reaped = 0;
    for (usize i = 0; i < processes_.size();)
    {
        const auto &process = processes_[i];
        if (process.parent_pid() == 0 &&
            (process.state() == ProcessState::zombie || process.state() == ProcessState::exited) &&
            reap_process(process.pid()).ok())
            ++reaped;
        else
            ++i;
    }
    return reaped;
}

bool SchedulerState::pipe_has_endpoint(u16 pipe, bool writable) const
{
    for (const auto &description : descriptions_)
        if (description.used && description.references != 0 && description.pipe == pipe &&
            (writable ? description.writable : description.readable))
            return true;
    return false;
}
Result<std::array<i32, 2>> SchedulerState::create_pipe(ProcessResources &resources, bool close_on_exec)
{
    std::array<i32, 2> fds{};
    usize count = 0;
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds) && count != 2; ++fd)
        if (!resources.files.contains(fd))
            fds[count++] = fd;
    if (count != 2)
        return Status::overflow("pipe needs two free descriptors");
    count = 0;
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
            ++count;
    if (count < 2)
        return Status::overflow("pipe needs two open descriptions");
    u16 pipe = invalid_description;
    for (usize i = 0; i < pipes_.size(); ++i)
        if (!pipes_[i].used)
        {
            pipe = static_cast<u16>(i);
            break;
        }
    if (pipe == invalid_description)
        return Status::overflow("pipe table full");
    pipes_[pipe] = PipeBuffer{.used = true};
    auto reader = allocate_description(OpenFileDescription{.used = true, .readable = true, .pipe = pipe});
    auto writer = allocate_description(OpenFileDescription{.used = true, .writable = true, .flags = 1, .pipe = pipe});
    static_cast<void>(install_descriptor(resources, fds[0], reader.value(), close_on_exec));
    static_cast<void>(install_descriptor(resources, fds[1], writer.value(), close_on_exec));
    return fds;
}
Result<usize> SchedulerState::read_pipe(u16 pipe, std::span<std::byte> out)
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::invalid_argument("invalid pipe");
    if (out.empty())
        return usize{0};
    auto &buffer = pipes_[pipe];
    if (buffer.size == 0)
    {
        if (!pipe_has_endpoint(pipe, true))
            return usize{0};
        static_cast<void>(block_current(ThreadWaitReason::pipe_read, pipe));
        return Status::would_block("pipe has no buffered data");
    }
    const auto count = out.size() < buffer.size ? out.size() : buffer.size;
    for (usize i = 0; i < count; ++i)
        out[i] = buffer.data[(buffer.head + i) % pipe_capacity];
    buffer.head = (buffer.head + count) % pipe_capacity;
    buffer.size -= count;
    wake_waiters(ThreadWaitReason::pipe_write, pipe);
    return count;
}
Result<usize> SchedulerState::write_pipe(u16 pipe, std::span<const std::byte> in)
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::invalid_argument("invalid pipe");
    if (in.empty())
        return usize{0};
    if (!pipe_has_endpoint(pipe, false))
        return Status::broken_pipe("pipe has no readers");
    auto &buffer = pipes_[pipe];
    const auto available = pipe_capacity - buffer.size;
    if (available == 0 || (in.size() <= pipe_capacity && in.size() > available))
    {
        static_cast<void>(block_current(ThreadWaitReason::pipe_write, pipe));
        return Status::would_block("pipe buffer is full");
    }
    const auto count = in.size() < available ? in.size() : available;
    for (usize i = 0; i < count; ++i)
        buffer.data[(buffer.head + buffer.size + i) % pipe_capacity] = in[i];
    buffer.size += count;
    wake_waiters(ThreadWaitReason::pipe_read, pipe);
    return count;
}
Status SchedulerState::block_current(ThreadWaitReason reason, u64 key)
{
    auto *process = find(current_pid());
    if (process == nullptr)
        return Status::not_found("no current process to block");
    auto *current = process->threads().empty() ? nullptr : &process->threads()[0];
    for (auto &thread : process->threads())
        if (thread.tid == current_tid(0))
            current = &thread;
    if (current == nullptr)
        return Status::not_found("no current thread to block");
    current->state = ProcessState::blocked;
    current->wait_reason = reason;
    current->wait_key = key;
    bool runnable = false;
    for (const auto &thread : process->threads())
        if (thread.state == ProcessState::runnable || thread.state == ProcessState::running)
            runnable = true;
    if (!runnable)
        process->set_state(ProcessState::blocked);
    return Status::success();
}
void SchedulerState::wake_waiters(ThreadWaitReason reason, u64 key)
{
    for (auto &process : processes_)
    {
        if (process.state() == ProcessState::zombie || process.state() == ProcessState::exited)
            continue;
        for (auto &thread : process.threads())
            if (thread.state == ProcessState::blocked && thread.wait_reason == reason && thread.wait_key == key)
            {
                thread.state = ProcessState::runnable;
                thread.wait_reason = ThreadWaitReason::none;
                thread.wait_key = 0;
                if (process.state() == ProcessState::blocked)
                    process.set_state(ProcessState::runnable);
            }
    }
}

} // namespace ok::sched
