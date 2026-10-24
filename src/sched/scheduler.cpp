
#include "ok/user/native_signal_config.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/net/native_tcp_stream.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/fs/vfs.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
#include "../user/signals/private_signal_scheduler_relocation.hpp"
#endif

namespace ok::sched
{
namespace
{

void count_resource_event(usize &count)
{
    if (count != ~usize{0}) ++count;
}

struct ResourceDrainAdmission
{
    bool &active;
    ~ResourceDrainAdmission() { active = false; }
};

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

// Linux getters accept a live nonleader TID as well as a PID. Completed
// nonleaders are no longer addressable, while a leader remains until reap.
const ProcessControlBlock *session_process(const SchedulerState &scheduler, ProcessId task)
{
    if (const auto *process = scheduler.find(task)) return process;
    for (const auto &process : scheduler.processes())
        for (const auto &thread : process.threads())
            if (thread.tid == task && thread.owner == process.pid() && thread.state != ProcessState::exited)
                return &process;
    return nullptr;
}

} // namespace

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string_view name, bool background)
    : pid_(pid), group_{.id = pid}, session_{.id = pid}, background_(background)
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
    if (next_task_id_ > max_task_id)
        return Status::overflow("process or thread identity exhausted");
    auto result = create_process_with_pid(next_task_id_, name, initial_context);
    if (result) ++next_task_id_;
    return result;
}

Result<ProcessId> SchedulerState::create_process_with_pid(ProcessId pid, std::string_view name,
                                                           arch::CpuContext initial_context)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (processes_.full())
    {
        return Status::overflow("process table capacity exceeded");
    }
    FixedString<max_process_name> validated_name;
    if (auto status = validated_name.assign(name); !status.ok())
    {
        return status;
    }
    if (!pid || pid > max_task_id || find(pid))
        return Status::invalid_argument("process identity is unavailable");
    ProcessControlBlock process{pid, validated_name.view()};
    if (auto status = process.threads().push_back(ThreadControlBlock{
            .tid = process.pid(),
            .owner = process.pid(),
            .state = ProcessState::created,
            .context = initial_context,
        });
        !status.ok())
    {
        return status;
    }
    initialize_resources(process.resources());
    auto slot = processes_.push_back_slot();
    if (!slot) return slot.status();
    *slot.value() = std::move(process);
    return pid;
}

Result<ProcessId> SchedulerState::create_init_process(arch::CpuContext initial_context)
{
    if (initial_context.mode != arch::PrivilegeMode::user)
        return Status::invalid_argument("init requires user execution");
    auto pid = create_process_with_pid(user_init_pid, "init", initial_context);
    if (!pid) return pid.status();
    auto *process = find(pid.value());
    process->set_execution(ProcessExecution::user_process);
    process->set_address_space_id(next_address_space_id_++);
    if (auto status = console_input_.bind_manager(pid.value()); !status) return status;
    return pid.value();
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
    // Generic kernel/model threads have no native entry resources to prepare.
    return create_thread_prepared(pid, initial_context,
        [](ThreadControlBlock &) { return Status::success(); });
}

Result<usize> SchedulerState::exit_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu)
{
    if (cpu >= cpu_count_)
        return Status::invalid_argument("thread retirement CPU is invalid");
    auto *process = find(pid);
    auto *thread = find_thread(pid, tid);
    if (!process || !thread)
        return Status::not_found("thread retirement identity not found");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited ||
        thread->state == ProcessState::exited)
        return Status::already_exists("thread is already retired");
    if (thread_running_on_other_cpu(tid, cpu))
        return Status::busy("thread is owned by another CPU");
    if (thread->entry_stack.pool || thread->kernel_stack_top)
        return Status::busy("thread-owned kernel stack must be retired first");
    thread->wait_registration.cancel();
    thread->native_deadline.cancel();
    thread->syscall_pending = false;
    thread->native_active = false;
    thread->wait_reason = ThreadWaitReason::none;
    thread->wait_key = 0;
    thread->state = ProcessState::exited;
    if (current_by_cpu_[cpu] == pid && current_thread_by_cpu_[cpu] == tid)
    {
        current_by_cpu_[cpu] = 0;
        current_thread_by_cpu_[cpu] = 0;
        cpu_stats_[cpu].current_pid = 0;
        cpu_stats_[cpu].current_tid = 0;
        if (cpu == 0) current_pid_ = 0;
    }
    usize live = 0;
    bool runnable = false, running = false;
    for (const auto &peer : process->threads())
        if (peer.state != ProcessState::exited)
        {
            ++live;
            running |= peer.state == ProcessState::running;
            runnable |= peer.state == ProcessState::runnable || peer.state == ProcessState::created;
        }
    process->set_state(running ? ProcessState::running : runnable ? ProcessState::runnable : ProcessState::blocked);
    thread->console_wait_epoch = 0;
    if (!live) retire_console_process(pid);
    return live;
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
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *process = find(pid);
    if (!process)
    {
        return Status::not_found("process not found");
    }
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::invalid_argument("cannot run an exited process");
    bool live = false;
    for (const auto &thread : process->threads()) live |= thread.state != ProcessState::exited;
    if (!live) return Status::invalid_argument("process has no live thread");
    process->set_state(ProcessState::runnable);
    for (auto &thread : process->threads())
    {
        if (thread.state == ProcessState::exited) continue;
        thread.wait_registration.cancel();
        thread.native_deadline.cancel();
        thread.wait_reason = ThreadWaitReason::none;
        thread.wait_key = 0;
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

Result<ProcessId> SchedulerState::dispatch_selected(smp::CpuId cpu, Result<ProcessId> next, ThreadId requested)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
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
    if (requested)
    {
        const auto *thread = find_thread(next.value(), requested);
        if (!thread || thread_running_on_other_cpu(requested, cpu) ||
            (thread->state != ProcessState::runnable &&
             !(thread->state == ProcessState::running && requested == previous_tid && next.value() == previous_pid)))
            return Status::invalid_argument("requested thread cannot be activated");
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
    auto selected_thread = requested ? Result<ThreadId>(requested)
                                    : pick_next_thread(*selected, selected->pid() == previous_pid ? previous_tid : 0, cpu);
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
    if (cpu == 0) current_pid_ = selected->pid();
    current_by_cpu_[cpu] = selected->pid();
    current_thread_by_cpu_[cpu] = selected_thread.value();
    ++cpu_stats_[cpu].dispatches;
    if (account_cpu)
    {
        ++cpu_stats_[cpu].busy_dispatches;
    }
    cpu_stats_[cpu].current_pid = selected->pid();
    cpu_stats_[cpu].current_tid = selected_thread.value();
    return selected->pid();
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
        OpenFileDescription{.used = true, .writable = true, .console = true, .flags = 1u, .path = FixedString<96>{"stdout"}};
    descriptions_[2] =
        OpenFileDescription{.used = true, .writable = true, .console = true, .flags = 1u, .path = FixedString<96>{"stderr"}};
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
Status SchedulerState::set_native_udp_service(net::NativeUdpService &service)
{
    if (native_udp_service_ == &service) return Status::success();
    for (const auto &description : descriptions_)
        if (description.used && description.native_udp_handle)
            return Status::busy("native UDP descriptions still own the endpoint service");
    native_udp_service_ = &service;
    return Status::success();
}
Status SchedulerState::set_native_tcp_service(net::NativeTcpStreamService &service)
{
    if (native_tcp_service_ == &service) return {};
    for (const auto &description : descriptions_)
        if (description.used && description.native_tcp_handle)
            return Status::busy("TCP descriptions still own their stream service");
    if (native_tcp_service_ && native_tcp_service_->active_streams())
        return Status::busy("TCP service reservations still own stream resources");
    if (native_tcp_service_)
    {
        const auto protocol = native_tcp_service_->endpoints().pools();
        if (protocol.connections || protocol.listeners || protocol.prepared)
            return Status::busy("TCP service protocol retirement has not drained");
    }
    if (auto status = service.endpoints().status(); !status) return status;
    native_tcp_service_ = &service; return {};
}
Status SchedulerState::set_shared_memory_files(user::native::SharedMemoryFiles &service)
{
    if (shared_memory_files_ == &service) return {};
    if (!service.ready()) return Status::not_initialized("shared memory file service is unavailable");
    for (const auto &description : descriptions_)
        if (description.used && description.native_shared_file)
            return Status::busy("shared memory descriptions still own their file service");
    if (shared_memory_files_)
    {
        const auto ledger = shared_memory_files_->ledger();
        if (ledger.files || ledger.retained_frames || ledger.reserved_cells)
            return Status::busy("shared memory aliases or retirement still own their file service");
    }
    shared_memory_files_ = &service;
    return {};
}
Result<u16> SchedulerState::allocate_description(OpenFileDescription description)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (description.reservation)
        return Status::invalid_argument("new description cannot import prepared reservation authority");
    if (description.unix_queue_references)
        return Status::invalid_argument("new description cannot import UNIX queue ownership");
    if (description.pty_endpoint)
    {
        if (!pty_transport_.readiness(description.pty_endpoint))
            return Status::bad_descriptor("PTY description has no live canonical provider");
        if (!description.readable || !description.writable || description.node || description.native_udp_handle ||
            description.native_tcp_handle || description.native_shared_file || description.unix_endpoint ||
            description.pipe != invalid_description || description.unix_write_pipe != invalid_description ||
            description.console || description.input_console)
            return Status::invalid_argument("PTY description cannot combine providers");
        for (const auto &existing : descriptions_)
            if (existing.used && existing.pty_endpoint == description.pty_endpoint)
                return Status::already_exists("PTY endpoint already belongs to a canonical description");
    }
    if (description.unix_endpoint)
    {
        const auto handle = description.unix_endpoint;
        const auto &endpoint = unix_endpoints_[handle.slot];
        if (!endpoint.used || endpoint.generation != handle.generation || endpoint.description != invalid_description)
            return Status::bad_descriptor("pathname UNIX description has no reserved endpoint");
        if (description.node || description.native_udp_handle || description.native_tcp_handle ||
            description.native_shared_file || description.console || description.input_console)
            return Status::invalid_argument("pathname UNIX description cannot combine providers");
    }
    if (description.native_shared_file)
    {
        if (!shared_memory_files_ || !shared_memory_files_->valid(description.native_shared_file))
            return Status::bad_descriptor("shared file description has no canonical owner");
        auto metadata = shared_memory_files_->info(description.native_shared_file);
        if (!metadata || !metadata.value().description)
            return Status::bad_descriptor("shared file description lost its provider hold");
        if (description.node || description.native_udp_handle || description.native_tcp_handle || description.unix_endpoint ||
            description.pipe != invalid_description || description.unix_write_pipe != invalid_description ||
            description.console || description.input_console)
            return Status::invalid_argument("shared file description cannot combine providers");
        for (const auto &existing : descriptions_)
            if (existing.used && existing.native_shared_file == description.native_shared_file)
                return Status::already_exists("shared file owner already belongs to an open description");
    }
    if (description.native_tcp_handle)
    {
        if (!native_tcp_service_ || !native_tcp_service_->valid(description.native_tcp_handle))
            return Status::bad_descriptor("TCP description has no canonical stream owner");
        if (description.native_udp_handle || description.native_shared_file || description.unix_endpoint || description.node || description.pipe != invalid_description ||
            description.unix_write_pipe != invalid_description || description.console || description.input_console)
            return Status::invalid_argument("TCP description cannot combine providers");
        for (const auto &existing : descriptions_)
            if (existing.used && existing.native_tcp_handle == description.native_tcp_handle)
                return Status::already_exists("TCP owner already belongs to an open description");
    }
    if (description.native_udp_handle && !native_udp_service_)
        return Status::not_initialized("native UDP description has no endpoint owner");
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
        {
            description.used = true;
            description.references = 0;
            descriptions_[i] = description;
            if (description.unix_endpoint)
                unix_endpoints_[description.unix_endpoint.slot].description = static_cast<u16>(i);
            return static_cast<u16>(i);
        }
    return Status::system_file_limit("open file description table full");
}
Status SchedulerState::install_descriptor(ProcessResources &resources, i32 fd, u16 description, bool close_on_exec)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *slot = resources.files.entry(fd);
    auto *open = open_description(description);
    if (slot == nullptr || open == nullptr)
        return Status::invalid_argument("invalid descriptor binding");
    if (slot->reservation)
        return Status::busy("descriptor belongs to a prepared operation");
    if (open->reservation)
        return Status::busy("canonical description belongs to a prepared operation");
    auto token = next_descriptor_token(); if (!token) return token.status();
    // Retain first: dup2 may replace the last other reference to this description.
    if (auto status = retain_description(description); !status.ok())
        return status;
    if (slot->used)
        static_cast<void>(close_descriptor(resources, fd));
    *slot = FileDescriptor{.used = true, .close_on_exec = close_on_exec, .description = description,
        .installation = token.value()};
    return Status::success();
}
Status SchedulerState::retain_description(u16 id)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *open = open_description(id);
    if (!open)
        return Status::bad_descriptor("invalid open description");
    if (open->reservation)
        return Status::busy("unpublished description cannot be externally retained");
    if (open->references == ~usize{0})
        return Status::overflow("open description reference limit exceeded");
    if (open->references == 0 && open->node)
        if (auto status = open->node->retain(); !status.ok())
            return status;
    ++open->references;
    return Status::success();
}
Status SchedulerState::release_description(u16 id)
{
    auto *open = open_description(id);
    if (!open || !open->references)
        return Status::bad_descriptor("open description has no reference to release");
    Status result = Status::success();
    --open->references;
    const auto pipe = open->pipe;
    const auto unix_write_pipe = open->unix_write_pipe;
    if (open->references == 0 && id >= 3)
    {
        const auto node = open->node;
        const auto endpoint = open->native_udp_handle;
        const auto stream = open->native_tcp_handle;
        const auto shared_file = open->native_shared_file;
        const auto unix_endpoint = open->unix_endpoint;
        const auto pty_endpoint = open->pty_endpoint;
        // Consume the exact external edge before a fallible provider callback.
        // A callback cannot resurrect this old OFD or erase a newly reused slot.
        *open = {};
        if (pty_endpoint)
        {
            const auto retired = pty_transport_.retire(pty_endpoint);
            if (!retired) result = retired.status();
            auto master = pty_endpoint; master.side = private_pty::Side::master;
            auto slave = pty_endpoint; slave.side = private_pty::Side::slave;
            for (const auto endpoint : {master, slave})
            {
                wake_waiters(ThreadWaitReason::pty_read, pty_wait_key(endpoint));
                wake_waiters(ThreadWaitReason::pty_write, pty_wait_key(endpoint));
            }
        }
        if (unix_endpoint)
            result = release_unix_endpoint(unix_endpoint);
        if (node)
        {
            const auto retired = node->release();
            if (result && !retired) result = retired;
        }
        if (shared_file)
        {
            const auto retired = shared_memory_files_ ? shared_memory_files_->release_description(shared_file)
                : Status::not_initialized("shared file owner is unavailable");
            if (result && !retired) result = retired;
        }
        if (stream)
        {
            const auto retired = native_tcp_service_ ? native_tcp_service_->close(stream)
                : Status::not_initialized("TCP endpoint owner is unavailable");
            if (result && !retired) result = retired;
        }
        if (endpoint)
        {
            const auto retired = native_udp_service_ ? native_udp_service_->close(endpoint)
                                                      : Status::not_initialized("UDP endpoint owner is unavailable");
            if (result && !retired) result = retired;
        }
    }
    auto wake_and_retire = [&](u16 index) {
        if (index >= pipes_.size()) return;
        wake_waiters(ThreadWaitReason::pipe_read, index);
        wake_waiters(ThreadWaitReason::pipe_write, index);
        if (!pipe_has_endpoint(index, false) && !pipe_has_endpoint(index, true))
        {
            const auto retired = retire_pipe(index);
            if (result && !retired) result = retired;
        }
    };
    if (pipe < pipes_.size())
    {
        wake_and_retire(pipe);
        if (unix_write_pipe != pipe)
            wake_and_retire(unix_write_pipe);
    }
    if (!collecting_unix_rights_)
    {
        const auto collected = collect_unix_rights();
        if (result && !collected) result = collected;
    }
    return result;
}
Status SchedulerState::close_descriptor(ProcessResources &resources, i32 fd)
{
    auto *slot = resources.files.entry(fd);
    if (slot == nullptr || !slot->used)
        return Status::invalid_argument("bad file descriptor");
    const auto description = slot->description;
    *slot = {};
    const auto *open = open_description(description);
    if (open && open->native_tcp_handle && native_tcp_service_)
        native_tcp_service_->descriptor_closed(&resources, fd, description);
    return release_description(description);
}
void SchedulerState::release_resources(ProcessResources &resources)
{
    static_cast<void>(drain_resources(resources));
}
void SchedulerState::record_resource_failure(Status status, i32 fd, u16 description, usize mount_id, bool cwd)
{
    if (status) return;
    count_resource_event(resource_drain_report_.failed_releases);
    if (resource_drain_report_.first_failure)
    {
        resource_drain_report_.first_failure = status;
        resource_drain_report_.first_failed_fd = fd;
        resource_drain_report_.first_failed_description = description;
        resource_drain_report_.first_failed_mount_id = mount_id;
        resource_drain_report_.first_failed_cwd = cwd;
    }
}
Status SchedulerState::consume_resources(ProcessResources &resources)
{
    if (resources.unix_receive_operations || resources.prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation still owns process resources");
    Status result{};
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds); ++fd)
        if (resources.files.contains(fd))
        {
            const auto description = resources.files.entry(fd)->description;
            const auto *open = open_description(description);
            const auto mount_id = open ? open->mount_id : 0;
            count_resource_event(resource_drain_report_.descriptors_consumed);
            const auto released = close_descriptor(resources, fd);
            record_resource_failure(released, fd, description, mount_id, false);
            if (result && !released) result = released;
        }
    resources.memory = {};
    resources.mappings = {};
    const auto cwd = resources.cwd_node;
    const auto mount_id = resources.cwd_mount_id;
    resources.cwd_node = nullptr;
    resources.cwd_mount_id = 0;
    if (cwd)
    {
        count_resource_event(resource_drain_report_.cwd_consumed);
        const auto released = cwd->release();
        record_resource_failure(released, -1, invalid_description, mount_id, true);
        if (result && !released) result = released;
    }
    return result;
}
Status SchedulerState::drain_resources(ProcessResources &resources)
{
    if (resource_drain_report_.active)
        return Status::busy("scheduler resource cleanup is already active");
    resource_drain_report_.active = true;
    ResourceDrainAdmission admission{resource_drain_report_.active};
    count_resource_event(resource_drain_report_.operations);
    return consume_resources(resources);
}
Status SchedulerState::drain_all_resources()
{
    // Refuse before consuming any other process or boot descriptor. A prepared
    // operation owns a PID-resolved reservation until publication/cancellation.
    if (active_unix_receives_ || active_descriptor_reservations_)
        return Status::busy("prepared descriptor operations prevent global resource drain");
    if (resource_drain_report_.active)
        return Status::busy("scheduler resource cleanup is already active");
    resource_drain_report_.active = true;
    ResourceDrainAdmission admission{resource_drain_report_.active};
    count_resource_event(resource_drain_report_.operations);
    auto consumed = consume_resources(boot_resources_);
    if (!consumed && resource_drain_report_.first_failure)
        record_resource_failure(consumed, -1, invalid_description, 0, false);
    for (auto &process : processes_)
    {
        consumed = consume_resources(process.resources());
        if (!consumed && resource_drain_report_.first_failure)
            record_resource_failure(consumed, -1, invalid_description, 0, false);
    }
    resource_drain_report_.descriptions_remaining = 0;
    resource_drain_report_.reservations_remaining = 0;
    for (usize id = 0; id < descriptions_.size(); ++id)
        if (descriptions_[id].used)
        {
            if (descriptions_[id].references)
                ++resource_drain_report_.descriptions_remaining;
            else if (id >= 3)
                ++resource_drain_report_.reservations_remaining;
        }
    if (!resource_drain_report_.first_failure) return resource_drain_report_.first_failure;
    if (resource_drain_report_.descriptions_remaining || resource_drain_report_.reservations_remaining)
        return Status::busy("open descriptions still own external pins or reservations");
    return {};
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
        if (cpu == 0) current_pid_ = 0;
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
Status SchedulerState::activate_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu)
{
    auto *process = find(pid);
    if (!tid || cpu >= cpu_count_ || !process ||
        (process->state() != ProcessState::runnable && process->state() != ProcessState::running) ||
        !process->can_run_on(cpu))
        return Status::invalid_argument("thread owner cannot be activated");
    auto selected = dispatch_selected(cpu, pid, tid);
    return selected ? Status::success() : selected.status();
}
Result<ProcessId> SchedulerState::fork_process(ProcessId parent, bool publish)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *source = find(parent);
    if (source == nullptr)
        return Status::not_found("parent process not found");
    if (source->resources().unix_receive_operations || source->resources().prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation cannot be cloned by fork");
    if (source->state() == ProcessState::zombie || source->state() == ProcessState::exited || source->threads().empty())
        return Status::invalid_argument("parent process is not alive");
    bool live = false;
    for (const auto &thread : source->threads()) live |= thread.state != ProcessState::exited;
    if (!live) return Status::invalid_argument("cannot fork a retired thread group");
    // Fork capacity is a retryable process-resource limit (EAGAIN), not an
    // arithmetic overflow. Check before retaining cwd or descriptor ownership.
    if (source->children().full() || processes_.full())
        return Status::would_block("fork process or child table capacity exceeded");
    // Preflight every duplicated OFD edge before publishing or retaining child state.
    for (const auto &slot : source->fd_table().entries()) if (slot.used)
    {
        const auto *open = open_description(slot.description);
        if (!open || !open->references) return Status::bad_descriptor("fork found a stale open description");
        usize increment{};
        for (const auto &other : source->fd_table().entries())
            increment += other.used && other.description == slot.description;
        if (open->references > ~usize{0} - increment)
            return Status::overflow("fork open description references would overflow");
    }
    const ThreadControlBlock *calling_thread = &source->threads()[0];
    for (const auto &thread : source->threads())
        if (thread.tid == current_tid(0) && current_pid(0) == parent)
            calling_thread = &thread;
    if (source->resources().cwd_node)
        if (auto status = source->resources().cwd_node->retain(); !status.ok())
            return status;
    auto child_pid = create_process(source->name(), calling_thread->context);
    if (!child_pid)
    {
        if (source->resources().cwd_node)
            static_cast<void>(source->resources().cwd_node->release());
        return child_pid.status();
    }
    auto *child = find(child_pid.value());
    release_resources(child->resources());
    // Native runtime materializes distinct mappings and tables before it makes
    // the child runnable. Copy only the scalar and retained scheduler resources.
    const auto &parent_resources = source->resources();
    auto &child_resources = child->resources();
    child_resources.personality = parent_resources.personality;
    child_resources.files = parent_resources.files;
    child_resources.memory = parent_resources.memory;
    child_resources.mappings = parent_resources.mappings;
    child_resources.cwd = parent_resources.cwd;
    child_resources.cwd_node = parent_resources.cwd_node;
    child_resources.cwd_mount_id = parent_resources.cwd_mount_id;
    child_resources.program_break_base = parent_resources.program_break_base;
    child_resources.program_break = parent_resources.program_break;
    child_resources.program_break_limit = parent_resources.program_break_limit;
    child_resources.next_mapping_address = parent_resources.next_mapping_address;
    child_resources.file_mode_mask = parent_resources.file_mode_mask;
    for (const auto &slot : child->fd_table().entries())
        if (slot.used)
            if (auto *description = open_description(slot.description))
                ++description->references;
    child->set_parent(parent);
    child->group_ = source->group_;
    child->session_ = source->session_;
    child->has_execed_ = false;
    child->console_epoch_ = console_input_.inherit(source->console_epoch_);
    child->set_credentials(source->credentials());
    child->set_execution(source->execution());
    child->set_address_space_id(next_address_space_id_++);
    child->set_background(source->background());
    child->set_priority(source->priority());
    child->set_cpu_affinity_mask(source->cpu_affinity_mask());
    child->threads()[0].priority = calling_thread->priority;
    child->threads()[0].tls = calling_thread->tls;
    child->threads()[0].native_signal_mask = calling_thread->native_signal_mask;
    child->threads()[0].native_frame = calling_thread->native_frame;
    child->threads()[0].native_active = false;
    child->threads()[0].syscall_pending = false;
    child->threads()[0].native_deadline = {};
    child->threads()[0].tls.clear_tid_address = 0;
    child->threads()[0].tls.set_tid_address = 0;
    child->threads()[0].tls.robust_list = 0;
    child->threads()[0].tls.robust_list_length = 0;
    child->threads()[0].wait_reason = ThreadWaitReason::none;
    static_cast<void>(source->add_child(child_pid.value()));
    if (publish) static_cast<void>(set_runnable(child_pid.value()));
    return child_pid;
}
Result<ProcessId> SchedulerState::process_group(ProcessId caller, i32 pid) const
{
    if (!find(caller)) return Status::no_process("calling process not found");
    if (pid < 0) return Status::no_process("process identity is negative");
    const auto *process = session_process(*this, pid == 0 ? caller : static_cast<ProcessId>(pid));
    if (!process) return Status::no_process("process group target not found");
    return process->process_group_id();
}
Result<ProcessId> SchedulerState::session_id(ProcessId caller, i32 pid) const
{
    if (!find(caller)) return Status::no_process("calling process not found");
    if (pid < 0) return Status::no_process("process identity is negative");
    const auto *process = session_process(*this, pid == 0 ? caller : static_cast<ProcessId>(pid));
    if (!process) return Status::no_process("session target not found");
    return process->session_id();
}
Status SchedulerState::set_process_group(ProcessId caller, i32 pid, i32 pgid)
{
    const auto *source = find(caller);
    if (!source) return Status::no_process("calling process not found");
    if (pid == 0) pid = static_cast<i32>(caller);
    if (pgid == 0) pgid = pid;
    // Match Linux ordering: a negative defaulted PGID precedes PID lookup.
    if (pgid < 0) return Status::invalid_argument("process group identity is negative");
    if (pid < 0) return Status::no_process("process identity is negative");
    auto *process = find(static_cast<ProcessId>(pid));
    if (!process)
        return session_process(*this, static_cast<ProcessId>(pid))
            ? Status::invalid_argument("only a thread group leader may change process groups")
            : Status::no_process("process group target not found");
    if (process->pid() != caller)
    {
        if (process->parent_pid() != caller) return Status::no_process("process is not a child of the caller");
        if (process->session_id() != source->session_id())
            return Status::permission("child is outside the calling session");
        if (process->has_execed_)
            return Status::denied("child has already replaced its image");
    }
    if (process->session_id() == process->pid())
        return Status::permission("a session leader cannot change process groups");
    if (pgid != pid)
    {
        bool same_session_group = false;
        for (const auto &member : processes_)
            if (member.process_group_id() == static_cast<ProcessId>(pgid) &&
                member.session_id() == source->session_id())
                same_session_group = true;
        if (!same_session_group) return Status::permission("process group does not exist in the calling session");
    }
    process->group_.id = static_cast<ProcessId>(pgid);
    if (console_input_.inherit(process->console_epoch_)) cancel_console_epoch(process->console_epoch_);
    return Status::success();
}
Result<ProcessId> SchedulerState::create_session(ProcessId caller)
{
    auto *process = find(caller);
    if (!process) return Status::no_process("calling process not found");
    if (process->session_id() == caller)
        return Status::permission("calling process is already a session leader");
    // Membership, rather than the lifetime of the original group leader,
    // reserves an identity. Retained zombies remain members until reap.
    for (const auto &member : processes_)
        if (member.process_group_id() == caller)
            return Status::permission("calling process identity already names a process group");
    process->session_.id = caller;
    process->group_.id = caller;
    if (console_input_.inherit(process->console_epoch_)) cancel_console_epoch(process->console_epoch_);
    return caller;
}
Status SchedulerState::replace_image(ProcessId pid, arch::CpuContext context, const MemoryMap &memory)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (auto status = arch::validate_architecture(context.architecture); !status.ok())
        return status;
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->resources().unix_receive_operations || process->resources().prepared_descriptor_operations)
        return Status::busy("prepared UNIX receive must finish before exec");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::invalid_argument("cannot exec an exited process");
    if (process->threads().empty())
        return Status::invalid_argument("process has no thread");
    bool live = false;
    for (const auto &thread : process->threads()) live |= thread.state != ProcessState::exited;
    if (!live) return Status::invalid_argument("cannot exec a retired thread group");
    for (const auto &thread : process->threads())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("entry stack references must be escrowed before exec");
    if (native_tcp_service_) native_tcp_service_->cancel_process(pid);
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds); ++fd)
    {
        const auto *slot = process->fd_table().entry(fd);
        if (slot->used && slot->close_on_exec)
            static_cast<void>(close_descriptor(process->resources(), fd));
    }
    process->memory_map() = memory;
    // Ordinary successful exec retains effective IDs and refreshes saved IDs.
    // Executable set-ID bits and secure-exec are not enabled by this transition.
    auto credentials = process->credentials();
    credentials.suid = credentials.euid;
    credentials.sgid = credentials.egid;
    process->set_credentials(credentials);
    process->resources().mappings = {};
    process->resources().program_break = process->resources().program_break_base;
    process->resources().next_mapping_address = 0x2000'0000;
    process->signals() = {};
    while (process->threads().size() > 1)
        static_cast<void>(process->threads().erase_at(1));
    auto &thread = process->threads()[0];
    thread.context = context;
    thread.tls = {};
    thread.native_deadline.cancel();
    thread.wait_registration.cancel();
    thread.console_wait_epoch = 0;
    thread.wait_reason = ThreadWaitReason::none;
    thread.wait_key = 0;
    thread.state = ProcessState::runnable;
    process->set_state(ProcessState::runnable);
    process->set_execution(context.mode == arch::PrivilegeMode::user ? ProcessExecution::user_process
                                                                     : ProcessExecution::kernel_thread);
    process->set_address_space_id(next_address_space_id_++);
    process->has_execed_ = true;
    clear_current(pid);
    return Status::success();
}
Status SchedulerState::exit_process(ProcessId pid, i32 code)
{
    return finish_process(pid, code, 0);
}
Status SchedulerState::terminate_process(ProcessId pid, u8 signal)
{
    // The common mechanism retains all 128 signals used by MIPS Linux ABIs.
    // The existing raw wait word remains lossy for signal 128.
    if (signal == 0 || signal > 128)
        return Status::invalid_argument("invalid termination signal");
    return finish_process(pid, 0, signal);
}
Status SchedulerState::finish_process(ProcessId pid, i32 code, u8 signal)
{
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->resources().unix_receive_operations || process->resources().prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation must finish before exit");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::already_exists("process has already exited");
    for (const auto &thread : process->threads())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("thread entry stacks must be retired before process exit");
    // Adoption is validated before the completion becomes observable.
    if (auto status = reparent_children_on_exit(pid); !status) return status;
    retire_console_process(pid);
    if (native_tcp_service_) native_tcp_service_->cancel_process(pid);
    process->set_exit_code(code);
    process->set_termination_signal(signal);
    process->set_state(ProcessState::zombie);
    for (auto &thread : process->threads())
    {
        thread.native_deadline.cancel();
        thread.wait_registration.cancel();
        thread.syscall_pending = false;
        thread.wait_reason = ThreadWaitReason::none;
        thread.wait_key = 0;
        thread.state = ProcessState::exited;
    }
    release_resources(process->resources());
    clear_current(pid);
    wake_waiters(ThreadWaitReason::child, pid);
    wake_waiters(ThreadWaitReason::child, 0);
    return Status::success();
}
Status SchedulerState::reparent_children_on_exit(ProcessId pid)
{
    auto *source = find(pid);
    if (!source) return Status::not_found("departing process not found");
    auto *init = find(user_adoption_target(pid));
    usize adopted = 0;
    for (const auto child_pid : source->children())
    {
        const auto *child = find(child_pid);
        if (!child || child->parent_pid() != pid)
            return Status::fault("departing process has an invalid child link");
        usize source_links = 0;
        for (const auto linked : source->children()) source_links += linked == child_pid;
        if (source_links != 1)
            return Status::fault("departing process has a duplicate child link");
        if (init && child->execution() == ProcessExecution::user_process)
        {
            for (const auto existing : init->children())
                if (existing == child_pid)
                    return Status::fault("init already owns departing child");
            ++adopted;
        }
    }
    if (init && init->children().size() + adopted > max_process_children)
        return Status::overflow("init child table full");
    for (const auto child_pid : source->children())
    {
        auto *child = find(child_pid);
        const bool to_init = init && child->execution() == ProcessExecution::user_process;
        child->set_parent(to_init ? user_init_pid : 0);
        if (to_init) static_cast<void>(init->add_child(child_pid));
    }
    source->children().clear();
    return Status::success();
}
ProcessId SchedulerState::user_adoption_target(ProcessId departing) const
{
    const auto *source = find(departing);
    const auto *init = find(user_init_pid);
    if (!source || departing == user_init_pid ||
        source->execution() != ProcessExecution::user_process || !init ||
        init->execution() != ProcessExecution::user_process ||
        source->state() == ProcessState::created ||
        init->state() == ProcessState::created ||
        init->state() == ProcessState::zombie || init->state() == ProcessState::exited)
        return 0;
    return user_init_pid;
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
    if (active_unix_receives_ || active_descriptor_reservations_)
        return Status::busy("prepared UNIX receives prevent process compaction");
    // A dead init is a terminal boot condition; PID 1 is never recycled.
    if (pid == user_init_pid && find(pid))
        return Status::busy("init must remain available for recovery diagnosis");
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        return Status::invalid_argument("process is not a zombie");
    for (const auto &thread : process->threads())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("thread entry stacks must be retired before reaping");
    if (process->resources().native_space.root != 0 || !process->resources().native_pages.empty())
        return Status::busy("native address space must be released before reaping");
    retire_console_process(pid);
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
    if (auto status = user::native::private_signal_before_process_compaction(*this); !status)
        return status;
#endif
    const auto parent = process->parent_pid();
    if (auto *owner = find(parent))
        static_cast<void>(owner->remove_child(pid));
    static_cast<void>(reparent_children(pid, 0));
    release_resources(process->resources());
    clear_current(pid);
    for (usize i = 0; i < processes_.size(); ++i)
        if (processes_[i].pid() == pid)
        {
            // The erased process has released its external resources. Shift
            // ownership once and clear the vacated final slot.
            for (usize next = i + 1; next < processes_.size(); ++next)
                processes_[next - 1] = std::move(processes_[next]);
            if (auto status = processes_.pop_back(); !status) return status;
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
            return user::native::private_signal_after_process_compaction(*this);
#else
            return Status::success();
#endif
        }
    return Status::not_found("process not found");
}

Status SchedulerState::detach_completed_child_to_reaper(ProcessId parent, ProcessId child)
{
    if (!parent || !child || parent == child)
        return Status::invalid_argument("completed child requires distinct nonzero identities");
    auto *owner = find(parent);
    auto *process = find(child);
    if (!owner || !process)
        return Status::not_found("completed child relationship not found");
    if (owner->state() == ProcessState::zombie || owner->state() == ProcessState::exited)
        return Status::invalid_argument("completed child parent has exited");
    if (process->parent_pid() != parent)
        return Status::invalid_argument("completed child parent does not match");
    usize links = 0;
    for (const auto id : owner->children()) links += id == child;
    if (links != 1)
        return Status::fault("completed child requires exactly one parent link");
    for (const auto &other : processes_)
        if (other.pid() != parent)
            for (const auto id : other.children())
                if (id == child)
                    return Status::fault("completed child is linked to another parent");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        return Status::invalid_argument("child completion has not been published");
    if (!process->children().empty())
        return Status::busy("completed child descendants must be reparented first");
    for (const auto &thread : process->threads())
        if (thread.state != ProcessState::exited || thread.entry_stack.pool || thread.kernel_stack_top ||
            thread.native_active || thread.syscall_pending || thread.native_deadline.armed() ||
            thread.wait_registration.active() || thread.wait_reason != ThreadWaitReason::none || thread.wait_key)
            return Status::busy("completed child threads must be retired first");
    for (smp::CpuId cpu = 0; cpu < cpu_count_; ++cpu)
        if (current_by_cpu_[cpu] == child)
            return Status::busy("completed child is still selected on a CPU");
    const auto &resources = process->resources();
    if (resources.native_space.root || !resources.native_pages.empty() || resources.files.open_count() ||
        resources.memory.area_count() || resources.cwd_node)
        return Status::busy("completed child resources must be released first");
    for (const auto &mapping : resources.mappings)
        if (mapping.used)
            return Status::busy("completed child mappings must be released first");
    // Every rejection above is read-only. Erasing one scalar child-list entry
    // cannot relocate either canonical process or thread storage.
    if (auto status = owner->remove_child(child); !status) return status;
    process->set_parent(0);
    return Status::success();
}

Status SchedulerState::reap_orphans_checked(OrphanReapProgress &progress)
{
    progress = {};
    for (usize i = 0; i < processes_.size();)
    {
        const auto &process = processes_[i];
        if (process.pid() == user_init_pid || process.parent_pid() != 0 ||
            (process.state() != ProcessState::zombie && process.state() != ProcessState::exited))
        {
            ++i;
            continue;
        }
        const auto pid = process.pid();
        if (auto status = reap_process(pid); !status)
        {
            progress.failed_pid = pid;
            progress.failed_after_erase = find(pid) == nullptr;
            progress.reaped += progress.failed_after_erase;
            return status;
        }
        ++progress.reaped;
    }
    return Status::success();
}

usize SchedulerState::reap_orphans()
{
    usize reaped = 0;
    for (usize i = 0; i < processes_.size();)
    {
        const auto &process = processes_[i];
        if (process.pid() != user_init_pid && process.parent_pid() == 0 &&
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
        if (description.used && description.references != 0 &&
            (writable ? (description.unix_write_pipe != invalid_description
                             ? description.unix_write_pipe : description.pipe)
                      : description.pipe) == pipe &&
            (writable ? description.writable : description.readable))
            return true;
    return false;
}
Result<PipeReadiness> SchedulerState::pipe_readiness(u16 pipe) const
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::bad_descriptor("invalid pipe");
    return PipeReadiness{.buffered = pipes_[pipe].size, .available = pipe_capacity - pipes_[pipe].size,
                         .readers = pipe_has_endpoint(pipe, false), .writers = pipe_has_endpoint(pipe, true)};
}
Result<std::array<i32, 2>> SchedulerState::create_pipe(ProcessResources &resources, bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("pipe status flags contain an access mode");
    // Global pipe/file resources precede descriptor-slot admission, as in
    // Linux create_pipe_files followed by get_unused_fd_flags. No state is
    // published until every bounded capacity check succeeds.
    usize count = 0;
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
            ++count;
    if (count < 2)
        return Status::system_file_limit("pipe needs two open descriptions");
    u16 pipe = invalid_description;
    for (usize i = 0; i < pipes_.size(); ++i)
        if (!pipes_[i].used)
        {
            pipe = static_cast<u16>(i);
            break;
        }
    if (pipe == invalid_description)
        return Status::system_file_limit("pipe table full");
    std::array<i32, 2> fds{};
    count = 0;
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds) && count != 2; ++fd)
        if (!resources.files.contains(fd))
            fds[count++] = fd;
    if (count != 2)
        return Status::process_file_limit("pipe needs two free descriptors");
    pipes_[pipe] = PipeBuffer{.used = true};
    auto reader = allocate_description(OpenFileDescription{.used = true, .readable = true, .flags = status_flags, .pipe = pipe});
    auto writer = allocate_description(OpenFileDescription{.used = true, .writable = true, .flags = status_flags | 1u, .pipe = pipe});
    static_cast<void>(install_descriptor(resources, fds[0], reader.value(), close_on_exec));
    static_cast<void>(install_descriptor(resources, fds[1], writer.value(), close_on_exec));
    return fds;
}
Result<std::array<i32, 2>> SchedulerState::create_unix_socketpair(ProcessResources &resources,
                                                                    bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("socket status flags contain an access mode");
    usize free_descriptions = 0;
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used) ++free_descriptions;
    if (free_descriptions < 2)
        return Status::system_file_limit("socket pair needs two open descriptions");
    std::array<u16, 2> buffers{invalid_description, invalid_description};
    usize found = 0;
    for (usize i = 0; i < pipes_.size() && found != 2; ++i)
        if (!pipes_[i].used) buffers[found++] = static_cast<u16>(i);
    if (found != 2)
        return Status::system_file_limit("socket pair needs two stream buffers");
    std::array<i32, 2> fds{};
    found = 0;
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds) && found != 2; ++fd)
        if (!resources.files.contains(fd)) fds[found++] = fd;
    if (found != 2)
        return Status::process_file_limit("socket pair needs two free descriptors");

    pipes_[buffers[0]] = PipeBuffer{.used = true};
    pipes_[buffers[1]] = PipeBuffer{.used = true};
    auto first = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[0], .unix_write_pipe = buffers[1]});
    auto second = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[1], .unix_write_pipe = buffers[0]});
    if (!first || !second)
    {
        if (first) *open_description(first.value()) = {};
        if (second) *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return !first ? first.status() : second.status();
    }
    if (auto status = install_descriptor(resources, fds[0], first.value(), close_on_exec); !status)
    {
        *open_description(first.value()) = {};
        *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return status;
    }
    if (auto status = install_descriptor(resources, fds[1], second.value(), close_on_exec); !status)
    {
        static_cast<void>(close_descriptor(resources, fds[0]));
        *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return status;
    }
    return fds;
}
Result<usize> SchedulerState::read_pipe(u16 pipe, std::span<std::byte> out, bool nonblocking)
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
        if (!nonblocking)
            static_cast<void>(block_current(ThreadWaitReason::pipe_read, pipe));
        return Status::would_block("pipe has no buffered data");
    }
    if (buffer.revision == ~u64{0})
        return Status::overflow("stream revision cannot wrap");
    u16 record = invalid_description;
    const auto count = unix_read_count(pipe, out.size(), record);
    for (usize i = 0; i < count; ++i)
        out[i] = buffer.data[(buffer.head + i) % pipe_capacity];
    consume_unix_bytes(pipe, count, record);
    return count;
}
Result<usize> SchedulerState::write_pipe(u16 pipe, std::span<const std::byte> in, bool nonblocking,
                                          bool atomic_request)
{
    auto result = write_pipe_fragment(pipe, in, atomic_request && in.size() <= pipe_capacity);
    if (!result && result.status().code() == StatusCode::would_block && !nonblocking)
        static_cast<void>(block_current(ThreadWaitReason::pipe_write, pipe));
    return result;
}
Result<usize> SchedulerState::write_pipe_fragment(u16 pipe, std::span<const std::byte> in, bool atomic_request)
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::invalid_argument("invalid pipe");
    if (in.empty())
        return usize{0};
    if (!pipe_has_endpoint(pipe, false))
        return Status::broken_pipe("pipe has no readers");
    auto &buffer = pipes_[pipe];
    if (buffer.revision == ~u64{0})
        return Status::overflow("stream revision cannot wrap");
    const auto available = pipe_capacity - buffer.size;
    if (available == 0 || (atomic_request && in.size() > available))
        return Status::would_block("pipe buffer is full");
    const auto count = in.size() < available ? in.size() : available;
    for (usize i = 0; i < count; ++i)
        buffer.data[(buffer.head + buffer.size + i) % pipe_capacity] = in[i];
    buffer.size += count;
    ++buffer.revision;
    wake_waiters(ThreadWaitReason::pipe_read, pipe);
    return count;
}
#include "unix_rights.inc"
#include "unix_path.inc"
#include "descriptor_reservation.inc"
#include "pty_pair.inc"
Status SchedulerState::block_current(ThreadWaitReason reason, u64 key)
{
    auto registration = register_current_wait(reason, key);
    return registration ? Status::success() : registration.status();
}
ThreadControlBlock *SchedulerState::find_thread(ProcessId pid, ThreadId tid)
{
    if (auto *process = find(pid))
        for (auto &thread : process->threads())
            if (thread.tid == tid)
                return &thread;
    return nullptr;
}
ThreadControlBlock *SchedulerState::current_thread()
{
    return find_thread(current_pid(0), current_tid(0));
}
Result<WaitTicket> SchedulerState::register_current_wait(ThreadWaitReason reason, u64 key)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (reason == ThreadWaitReason::none)
        return Status::invalid_argument("wait requires a reason");
    auto *process = find(current_pid());
    if (process == nullptr)
        return Status::not_found("no current process to block");
    auto *current = current_thread();
    if (current == nullptr || current->owner != process->pid())
        return Status::not_found("no current thread to block");
    if (current->state != ProcessState::running)
        return Status::busy("wait owner is not running");
    auto ticket = current->wait_registration.arm(process->pid(), current->tid);
    if (!ticket)
        return ticket.status();
    current->state = ProcessState::blocked;
    current->wait_reason = reason;
    current->wait_key = key;
    bool runnable = false;
    for (const auto &thread : process->threads())
        if (thread.state == ProcessState::runnable || thread.state == ProcessState::running)
            runnable = true;
    if (!runnable)
        process->set_state(ProcessState::blocked);
    return ticket;
}
bool SchedulerState::complete_wait(WaitTicket ticket, WaitCompletion completion)
{
    auto *process = find(ticket.owner);
    auto *thread = find_thread(ticket.owner, ticket.thread);
    if (!process || !thread || process->state() == ProcessState::zombie ||
        process->state() == ProcessState::exited || thread->state != ProcessState::blocked ||
        !thread->wait_registration.complete(ticket, completion))
        return false;
    thread->native_deadline.cancel();
    thread->wait_reason = ThreadWaitReason::none;
    thread->wait_key = 0;
    thread->state = ProcessState::runnable;
    if (process->state() == ProcessState::blocked)
        process->set_state(ProcessState::runnable);
    return true;
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
                static_cast<void>(complete_wait(thread.wait_registration.ticket(), WaitCompletion::ready));
            }
    }
}

#include "console_input.inc"
Status SchedulerState::close_shutdown_admission()
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission was already closed");
    if (active_unix_receives_ || active_descriptor_reservations_ || resource_drain_report_.active)
        return Status::busy("prepared resource operation prevents shutdown admission closure");
    shutdown_admission_closed_ = true;
    return {};
}

} // namespace ok::sched
