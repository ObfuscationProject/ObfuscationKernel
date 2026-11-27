
#include "ok/user/native_signal_config.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/net/native_tcp_stream.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/smp/bringup.hpp"
#include "resource_drain.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
#include "../user/signals/private_signal_scheduler_relocation.hpp"
#endif

namespace ok::sched
{
namespace
{

bool process_has_runnable_thread(const ProcessControlBlock &process)
{
    for (const auto &thread : process.live_threads())
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
    if (!process.can_run_on(cpu) || process.job_control().stopped)
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

// Holds a slot's task lock unless this CPU holds it already: the scheduler's
// entry points take it, and the transitions they share run inside them.
class TaskHold final
{
  public:
    explicit TaskHold(ProcessSlot &slot) : slot_(slot), taken_(!slot.task.owned_by_current_cpu())
    {
        if (taken_) slot_.task.lock();
    }
    ~TaskHold()
    {
        if (taken_) slot_.task.unlock();
    }
    TaskHold(const TaskHold &) = delete;
    TaskHold &operator=(const TaskHold &) = delete;

  private:
    ProcessSlot &slot_;
    bool taken_;
};

// Holds the task table unless this CPU holds it already: a group send holds
// it across its callbacks (spec 4.7), which may reach slot allocation or
// release.
class TableHold final
{
  public:
    explicit TableHold(TaskTableLock &lock) : lock_(lock), taken_(!lock.owned_by_current_cpu())
    {
        if (taken_) lock_.lock();
    }
    ~TableHold()
    {
        if (taken_) lock_.unlock();
    }
    TableHold(const TableHold &) = delete;
    TableHold &operator=(const TableHold &) = delete;

  private:
    TaskTableLock &lock_;
    bool taken_;
};

// A thread the queue may hold: runnable, or blocked with a probe pending.
bool queue_admits_state(const ThreadControlBlock &thread)
{
    return thread.state == ProcessState::runnable || (thread.state == ProcessState::blocked && thread.probe);
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
        for (const auto &thread : process.live_threads())
            if (thread.tid == task && thread.owner == process.pid())
                return &process;
    return nullptr;
}

} // namespace

ProcessControlBlock::ProcessControlBlock(ProcessId pid, std::string_view name, bool background)
    : pid_(pid), group_{.id = pid}, session_{.id = pid}, background_(background)
{
    static_cast<void>(name_.assign(name));
}

Result<ProcessId> RoundRobinPolicy::pick_next(ConstProcessRange processes, ProcessId current, smp::CpuId cpu)
{
    if (processes.empty())
    {
        return Status::not_found("no processes available");
    }

    // Slot order; the search starts after the current process's slot.
    constexpr usize count = ConstProcessRange::capacity();
    usize start = 0;
    for (usize i = 0; i < count; ++i)
        if (const auto *process = processes.at(i); process && process->pid() == current)
        {
            start = (i + 1) % count;
            break;
        }

    u8 best_priority = scheduler_min_priority;
    bool found = false;
    for (usize offset = 0; offset < count; ++offset)
    {
        const auto *process = processes.at((start + offset) % count);
        if (!process || !process_allowed_on(*process, cpu, current))
        {
            continue;
        }
        if (!found || process->priority() > best_priority)
        {
            best_priority = process->priority();
            found = true;
        }
    }

    if (!found)
    {
        return Status::would_block("no runnable process");
    }

    for (usize offset = 0; offset < count; ++offset)
    {
        const auto *process = processes.at((start + offset) % count);
        if (process && process_allowed_on(*process, cpu, current) && process->priority() == best_priority)
        {
            return process->pid();
        }
    }

    return Status::would_block("no runnable process");
}

Result<ProcessId> SchedulerState::create_process(std::string_view name, arch::CpuContext initial_context)
{
    smp::check_exclusive("SchedulerState::create_process");
    if (next_task_id_ > max_task_id)
        return Status::overflow("process or thread identity exhausted");
    auto result = create_process_with_pid(next_task_id_, name, initial_context);
    if (result) ++next_task_id_;
    return result;
}

Result<ProcessId> SchedulerState::create_process_with_pid(ProcessId pid, std::string_view name,
                                                           arch::CpuContext initial_context)
{
    smp::check_exclusive("SchedulerState::create_process_with_pid");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    FixedString<max_process_name> validated_name;
    if (auto status = validated_name.assign(name); !status.ok())
    {
        return status;
    }
    if (!pid || pid > max_task_id || slot_of(pid))
        return Status::invalid_argument("process identity is unavailable");
    auto *slot = claim_process_slot();
    if (!slot)
    {
        return Status::overflow("process table capacity exceeded");
    }
    // Built in place: the slot is an embryo, so nothing else reaches its PCB.
    auto &process = slot->pcb;
    process = ProcessControlBlock{pid, validated_name.view()};
    process.process_slot_ = static_cast<u16>(slot - slots_.data());
    process.threads_[0] = ThreadControlBlock{
        .tid = process.pid(),
        .owner = process.pid(),
        .state = ProcessState::created,
        .context = initial_context,
        .slot_state = ThreadSlotState::live,
    };
    for (auto &mask : slot->thread_affinity) mask.store(cpu_affinity_any, std::memory_order_relaxed);
    // A reused slot starts unplaced and unflagged.
    for (auto &word : slot->on_cpu) word.store(no_cpu, std::memory_order_relaxed);
    for (auto &word : slot->queued_cpu) word.store(no_cpu, std::memory_order_relaxed);
    slot->run_flags.store(0, std::memory_order_relaxed);
    slot->affinity.store(cpu_affinity_any, std::memory_order_relaxed);
    slot->transition = GroupTransition::none;
    slot->transition_tid = 0;
    slot->exit_request = {};
    initialize_resources(process.resources());
    {
        // Published last: lookups see the slot only once the PCB is complete.
        const TableHold table{task_table_};
        publish_slot(*slot, pid);
    }
    return pid;
}

Result<ProcessId> SchedulerState::create_init_process(arch::CpuContext initial_context)
{
    smp::check_exclusive("SchedulerState::create_init_process");
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
    smp::check_exclusive("SchedulerState::create_user_process");
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
    smp::check_exclusive("SchedulerState::create_background_process");
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
    smp::check_exclusive("SchedulerState::create_thread");
    // Generic kernel/model threads have no native entry resources to prepare.
    return create_thread_prepared(pid, initial_context,
        [](ThreadControlBlock &) { return Status::success(); });
}

Result<usize> SchedulerState::exit_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu)
{
    smp::check_exclusive("SchedulerState::exit_thread");
    smp::check_drained(pid, "SchedulerState::exit_thread");
    if (cpu >= cpu_count_)
        return Status::invalid_argument("thread retirement CPU is invalid");
    auto *process = find(pid);
    auto *thread = find_thread(pid, tid);
    if (!process || !thread)
        return Status::not_found("thread retirement identity not found");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited ||
        thread->state == ProcessState::exited)
        return Status::already_exists("thread is already retired");
    if (thread_running_on_other_cpu(*thread, cpu))
        return Status::busy("thread is owned by another CPU");
    if (thread->entry_stack.pool || thread->kernel_stack_top)
        return Status::busy("thread-owned kernel stack must be retired first");
    {
        auto &slot = task_slot(*process);
        TaskHold hold{slot};
        thread->wait_registration.cancel();
        thread->native_deadline.cancel();
        forget_wait_deadline_locked(process->thread_index(*thread));
        thread->syscall_pending = false;
        thread->native_active = false;
        thread->wait_reason = ThreadWaitReason::none;
        thread->wait_key = 0;
        thread->probe = false;
        thread->state = ProcessState::exited;
        thread->slot_state = ThreadSlotState::exited;
        forget_queued_locked(slot, *thread);
    }
    release_wait_links(*process, *thread);
    if (cpu_slots_[cpu].pid == pid && cpu_slots_[cpu].tid == tid)
        vacate_cpu_slot(cpu);
    const usize live = process->live_count();
    thread->console_wait_epoch = 0;
    // The last thread's exit is Linux group_dead: disassociate_ctty(1) runs
    // before the lease retires, as in finish_process.
    if (!live)
    {
        hangup_exiting_leader(*process);
        retire_console_process(pid);
    }
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
    for (auto &thread : process->thread_records())
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
    auto &slot = task_slot(*process);
    TaskHold hold{slot};
    process->set_cpu_affinity_mask(effective_mask);
    slot.affinity.store(effective_mask, std::memory_order_release);
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
    // Every CPU slot is vacated (its pin dropped) before the count changes.
    for (usize cpu = 0; cpu < cpu_slots_.size(); ++cpu)
        if (cpu_slots_[cpu].occupied() || cpu_slots_[cpu].process) vacate_cpu_slot(cpu);
    cpu_count_ = cpu_count;
    for (auto &slot : slots_)
    {
        if (!slot.visible()) continue;
        TaskHold hold{slot};
        for (auto &thread : slot.pcb.thread_records())
            set_on_cpu_locked(slot, thread, no_cpu);
    }
    online_cpus_ &= configured_cpu_mask();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        run_queues_.get(cpu).set_closed(!cpu_online(cpu));
    // Entries of removed CPUs are placed again; a stale one is dropped here.
    for (auto cpu = static_cast<smp::CpuId>(cpu_count_); cpu < smp::max_cpus; ++cpu)
        drain_queue(cpu);
    return Status::success();
}

Status SchedulerState::set_online_cpus(u32 mask)
{
    if ((mask & 1u) == 0)
        return Status::invalid_argument("the boot CPU must stay online");
    if ((mask & ~static_cast<u32>(configured_cpu_mask())) != 0)
        return Status::invalid_argument("online CPU is not configured");
    online_cpus_ = mask;
    // An offline CPU's queue refuses pushes; entries already there stay.
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        run_queues_.get(cpu).set_closed(!cpu_online(cpu));
    return Status::success();
}

Status SchedulerState::offline_cpu(smp::CpuId cpu)
{
    if (cpu == 0)
        return Status::invalid_argument("the boot CPU must stay online");
    if (cpu >= cpu_count_)
        return Status::invalid_argument("offline CPU is not configured");
    // A process mask is kernel placement policy: it must keep an online CPU.
    // Thread masks do not refuse; such a thread's affinity breaks instead.
    const u32 remaining = online_cpus_ & ~(1u << cpu);
    for (const auto &process : process_range())
        if (process.state() != ProcessState::zombie && process.state() != ProcessState::exited &&
            (process.cpu_affinity_mask() & remaining) == 0)
            return Status::busy("a process allows no other online CPU");
    online_cpus_ = remaining;
    // The closed queue refuses new entries. Each remaining entry is popped
    // once and placed again on an online queue; one whose thread no longer
    // names this queue is stale and dropped. A thread none of whose CPUs
    // stays online gets its affinity broken there.
    run_queues_.get(cpu).set_closed(true);
    drain_queue(cpu);
    return Status::success();
}

void SchedulerState::drain_queue(smp::CpuId cpu)
{
    auto &queue = run_queues_.get(cpu);
    while (const auto handle = queue.pop())
    {
        if (!handle->valid() || handle->index >= max_thread_slots) continue;
        auto &slot = slots_[handle->index / max_threads_per_process];
        auto &thread = slot.pcb.threads_[handle->index % max_threads_per_process];
        smp::CpuId placed = no_cpu;
        {
            TaskHold hold{slot};
            if (thread.slot_state == ThreadSlotState::free || thread.tid != handle->tid || thread.queued_cpu != cpu)
                continue;
            set_queued_locked(slot, thread, no_cpu);
            if (thread.on_cpu == no_cpu) placed = enqueue_locked(slot, thread, true);
        }
        notify_cpu(placed);
    }
}

Status SchedulerState::online_cpu(smp::CpuId cpu)
{
    if (cpu >= cpu_count_)
        return Status::invalid_argument("online CPU is not configured");
    online_cpus_ |= 1u << cpu;
    run_queues_.get(cpu).set_closed(false);
    return Status::success();
}

Status SchedulerState::enqueue_thread(ProcessId pid, ThreadId tid)
{
    smp::check_exclusive("SchedulerState::enqueue_thread");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *process = find(pid);
    auto *thread = find_thread(pid, tid);
    if (!process || !thread)
        return Status::not_found("queued thread not found");
    auto &slot = task_slot(*process);
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{slot};
        if (thread->on_cpu != no_cpu)
            return Status::success();
        // A probed blocked thread may be queued too (its resume re-evaluates).
        if (!queue_admits_state(*thread) || process->state() == ProcessState::zombie ||
            process->state() == ProcessState::exited)
            return Status::invalid_argument("only a runnable or probed thread can be queued");
        // A stopped process is never queued; continue_process queues it.
        if (process->job_.stopped)
            return Status::invalid_argument("a stopped process is not queued");
        if (thread->queued_cpu != no_cpu)
            return Status::success();
        placed = enqueue_locked(slot, *thread, true);
        if (placed == no_cpu) return Status::overflow("no run queue accepted the thread");
    }
    notify_cpu(placed);
    return Status::success();
}

smp::CpuId SchedulerState::select_cpu(ProcessControlBlock &process, ThreadControlBlock &thread)
{
    // Last allowed CPU gone offline: the thread mask resets (broken affinity).
    static_cast<void>(repair_affinity(process, thread));
    return select_from(thread, effective_affinity(process, thread) & online_cpus_);
}

smp::CpuId SchedulerState::select_from(const ThreadControlBlock &thread, u32 allowed) const
{
    // A thread that never ran has no cache to return to.
    if (thread.dispatch_count != 0 && thread.last_cpu < smp::max_cpus && (allowed & (1u << thread.last_cpu)) != 0)
        return thread.last_cpu;
    smp::CpuId best = no_cpu;
    usize shortest = 0;
    for (smp::CpuId cpu = 0; cpu < cpu_count_; ++cpu)
        if ((allowed & (1u << cpu)) != 0)
            if (const auto length = run_queues_.get(cpu).size(); best == no_cpu || length < shortest)
            {
                best = cpu;
                shortest = length;
            }
    // Never an offline CPU: CPU 0 is always online. Only a process mask that
    // names no configured CPU gets here (thread masks were repaired above).
    return best != no_cpu ? best : 0;
}

smp::CpuId SchedulerState::enqueue_locked(ProcessSlot &slot, ThreadControlBlock &thread, bool requeue)
{
    auto &process = slot.pcb;
    if (thread.queued_cpu != no_cpu || shutdown_admission_closed_) return no_cpu;
    // Kernel_thread records keep the Kernel::tick policy; an entry that is
    // only placed again keeps its queue whatever its process.
    if (!requeue && process.execution() != ProcessExecution::user_process) return no_cpu;
    if (thread.slot_state != ThreadSlotState::live || !may_queue(slot, thread) ||
        process.state_ == ProcessState::zombie || process.state_ == ProcessState::exited ||
        slot.state() != ProcessLifecycle::live)
        return no_cpu;
    static_cast<void>(repair_affinity(process, thread));
    u32 allowed = effective_affinity(process, thread) & online_cpus_;
    const auto handle = handle_of(process, thread);
    for (;;)
    {
        const auto cpu = select_from(thread, allowed);
        if (run_queues_.get(cpu).push(handle))
        {
            set_queued_locked(slot, thread, cpu);
            return cpu;
        }
        // A closed queue (its CPU went offline meanwhile) or a full one.
        if (allowed == 0 || cpu == 0)
        {
            smp::count_check(smp::KernelCheck::queue, "no run queue accepted a runnable thread");
            return no_cpu;
        }
        allowed &= ~(1u << cpu);
    }
}

void SchedulerState::notify_cpu(smp::CpuId cpu)
{
    if (cpu >= smp::max_cpus) return;
    const auto self = smp::current_cpu();
    if (cpu != self && self < smp::max_cpus) smp::detail::owner_add(smp::evidence_cpus[self].remote_wakes);
    if constexpr (smp::concurrent_locks)
    {
        // Waker side of the idle handshake: the push is visible before the
        // idle flag is read; the idle side fences between its store and its
        // queue loads.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (cpu == self || !idle_[cpu].load(std::memory_order_relaxed)) return;
        wake_requests_[cpu].store(1, std::memory_order_release);
        if (self < smp::max_cpus) smp::detail::owner_add(smp::evidence_cpus[self].wake_ipis);
        smp::send_ipi(cpu, smp::ipi_reschedule);
    }
}

void SchedulerState::set_queued_locked(ProcessSlot &slot, ThreadControlBlock &thread, smp::CpuId cpu)
{
    if (!slot.task.owned_by_current_cpu())
        smp::count_check(smp::KernelCheck::queue, "queued_cpu written without the task lock");
    thread.queued_cpu = cpu;
    if (const auto index = slot.pcb.slot_of(thread); index < max_threads_per_process)
        slot.queued_cpu[index].store(cpu, std::memory_order_release);
}

void SchedulerState::set_on_cpu_locked(ProcessSlot &slot, ThreadControlBlock &thread, smp::CpuId cpu)
{
    if (!slot.task.owned_by_current_cpu())
        smp::count_check(smp::KernelCheck::queue, "on_cpu written without the task lock");
    thread.on_cpu = cpu;
    if (const auto index = slot.pcb.slot_of(thread); index < max_threads_per_process)
        slot.on_cpu[index].store(cpu, std::memory_order_release);
}

bool SchedulerState::may_queue(const ProcessSlot &slot, const ThreadControlBlock &thread)
{
    // A frozen group runs only its claimer (spec 4.8); a pending exit is
    // admitted in any state, stopped or not, so that a thread applies it.
    const auto flags = slot.run_flags.load(std::memory_order_relaxed);
    if ((flags & static_cast<u32>(RunFlag::frozen)) != 0) return false;
    if ((flags & static_cast<u32>(RunFlag::pending_exit)) != 0) return true;
    return !slot.pcb.job_.stopped && queue_admits_state(thread);
}

bool SchedulerState::affinity_admits(const ProcessControlBlock &process, smp::CpuId cpu) const
{
    return process.can_run_on(cpu) || (cpu == 0 && (process.cpu_affinity_mask() & configured_cpu_mask()) == 0);
}

bool SchedulerState::thread_admits(const ProcessControlBlock &process, const ThreadControlBlock &thread,
                                   smp::CpuId cpu) const
{
    if (cpu >= smp::max_cpus) return false;
    const u16 allowed = effective_affinity(process, thread);
    return (allowed & (1u << cpu)) != 0 || (cpu == 0 && (allowed & configured_cpu_mask()) == 0);
}

bool SchedulerState::repair_affinity(ProcessControlBlock &process, ThreadControlBlock &thread)
{
    const u16 allowed = effective_affinity(process, thread);
    // Only the thread mask breaks; a process mask keeps an online CPU (offline_cpu).
    if ((allowed & online_cpus_) != 0 || thread_affinity(process, thread) == cpu_affinity_any ||
        (process.cpu_affinity_mask() & online_cpus_) == 0)
        return false;
    smp::CpuId first = 0;
    while (first + 1u < smp::max_cpus && (allowed & (1u << first)) == 0) ++first;
    store_thread_affinity(process, thread, cpu_affinity_any);
    affinity_broken_[affinity_broken_total_ % affinity_broken_.size()] = {process.pid(), thread.tid, first};
    ++affinity_broken_total_;
    return true;
}

Status SchedulerState::set_thread_affinity(ProcessControlBlock &process, ThreadControlBlock &thread, u16 mask)
{
    if ((process.cpu_affinity_mask() & mask & online_cpus_) == 0)
        return Status::invalid_argument("affinity names no online CPU the process allows");
    store_thread_affinity(process, thread, mask);
    return {};
}

ThreadControlBlock *SchedulerState::find_thread_by_tid(ThreadId tid)
{
    smp::check_exclusive("SchedulerState::find_thread_by_tid");
    if (!tid) return nullptr;
    for (auto &process : process_range())
    {
        if (process.state() == ProcessState::zombie || process.state() == ProcessState::exited) continue;
        for (auto &thread : process.live_threads())
            if (thread.tid == tid) return &thread;
    }
    return nullptr;
}

std::optional<AffinityBroken> SchedulerState::take_affinity_broken()
{
    if (affinity_broken_taken_ == affinity_broken_total_) return std::nullopt;
    // Older records than the ring holds are counted, not reported.
    if (affinity_broken_total_ - affinity_broken_taken_ > affinity_broken_.size())
        affinity_broken_taken_ = affinity_broken_total_ - affinity_broken_.size();
    return affinity_broken_[affinity_broken_taken_++ % affinity_broken_.size()];
}

Result<ProcessId> SchedulerState::schedule_kernel_thread_on_cpu(smp::CpuId cpu)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (cpu >= cpu_count_) return Status::invalid_argument("scheduler cpu id out of range");
    const auto current = current_pid(cpu);
    const auto range = process_range();
    constexpr usize count = ProcessRange::capacity();
    usize start = 0;
    for (usize i = 0; i < count; ++i)
        if (const auto *process = range.at(i); process && process->pid() == current) start = (i + 1) % count;
    // Highest priority first, then round robin after the current record.
    const ProcessControlBlock *best = nullptr;
    for (usize offset = 0; offset < count; ++offset)
    {
        const auto *process = range.at((start + offset) % count);
        if (!process || process->execution() != ProcessExecution::kernel_thread ||
            !process_allowed_on(*process, cpu, current))
            continue;
        if (!best || process->priority() > best->priority()) best = process;
    }
    if (!best) return Status::would_block("no runnable kernel thread");
    return dispatch_selected(cpu, best->pid());
}

bool SchedulerState::eligible_on(ThreadHandle handle, smp::CpuId queue, smp::CpuId cpu) const
{
    const auto *thread = thread_at(handle);
    if (!thread) return false;
    const auto &slot = slots_[handle.index / max_threads_per_process];
    const auto &process = slot.pcb;
    if (!slot.visible() || process.state() == ProcessState::zombie || process.state() == ProcessState::exited ||
        process.job_.stopped || slot.flagged(RunFlag::frozen))
        return false;
    return thread->queued_cpu == queue && thread->state == ProcessState::runnable && thread->on_cpu == no_cpu &&
           thread_admits(process, *thread, cpu);
}

std::optional<ThreadRef> SchedulerState::dequeue_thread(smp::CpuId cpu)
{
    smp::check_exclusive("SchedulerState::dequeue_thread");
    if (shutdown_admission_closed_ || !cpu_online(cpu))
        return std::nullopt;
    auto &own = run_queues_.get(cpu);
    // Bounded by the entries present on entry; moved entries never return here.
    for (auto budget = own.size(); budget != 0; --budget)
    {
        const auto handle = own.pop();
        if (!handle)
            break;
        if (!handle->valid() || handle->index >= max_thread_slots) continue;
        auto &slot = slots_[handle->index / max_threads_per_process];
        auto &thread = slot.pcb.threads_[handle->index % max_threads_per_process];
        auto *process = &slot.pcb;
        smp::CpuId placed = no_cpu;
        std::optional<ThreadRef> taken{};
        {
            TaskHold hold{slot};
            if (thread.slot_state == ThreadSlotState::free || thread.tid != handle->tid || thread.queued_cpu != cpu)
                continue;
            set_queued_locked(slot, thread, no_cpu);
            if (thread.state != ProcessState::runnable || thread.on_cpu != no_cpu ||
                process->state() == ProcessState::zombie || process->state() == ProcessState::exited ||
                process->job_.stopped || slot.flagged(RunFlag::frozen))
                continue;
            if (!thread_admits(*process, thread, cpu))
                placed = enqueue_locked(slot, thread, true);
            else
                taken = ThreadRef{thread.owner, thread.tid};
        }
        notify_cpu(placed);
        if (taken) return taken;
    }
    // The own queue lock is released; victims are only try-locked.
    u32 tried = 1u << cpu;
    for (;;)
    {
        smp::CpuId victim = no_cpu;
        usize longest = 0;
        for (smp::CpuId other = 0; other < cpu_count_; ++other)
            if (cpu_online(other) && (tried & (1u << other)) == 0)
                if (const auto length = run_queues_.get(other).size(); length > longest)
                {
                    victim = other;
                    longest = length;
                }
        if (victim == no_cpu)
            return std::nullopt;
        tried |= 1u << victim;
        const auto stolen = run_queues_.get(victim).try_take_first(
            [&](ThreadHandle handle) { return eligible_on(handle, victim, cpu); });
        if (stolen)
        {
            auto &slot = slots_[stolen->index / max_threads_per_process];
            auto &thread = slot.pcb.threads_[stolen->index % max_threads_per_process];
            TaskHold hold{slot};
            set_queued_locked(slot, thread, no_cpu);
            return ThreadRef{thread.owner, thread.tid};
        }
    }
}

bool SchedulerState::steal_eligible(ThreadHandle handle, smp::CpuId victim, smp::CpuId cpu) const
{
    // Lock-free loads only: the victim's queue lock is held, and the entry
    // is validated again under its task lock once taken.
    if (!handle.valid() || handle.index >= max_thread_slots || cpu >= smp::max_cpus) return false;
    const auto &slot = slots_[handle.index / max_threads_per_process];
    const auto index = handle.index % max_threads_per_process;
    if (slot.state() != ProcessLifecycle::live) return false;
    const auto flags = slot.run_flags.load(std::memory_order_acquire);
    if ((flags & static_cast<u32>(RunFlag::frozen)) != 0 ||
        ((flags & static_cast<u32>(RunFlag::stopped)) != 0 && (flags & static_cast<u32>(RunFlag::pending_exit)) == 0))
        return false;
    if (slot.queued_cpu[index].load(std::memory_order_acquire) != victim ||
        slot.on_cpu[index].load(std::memory_order_acquire) != no_cpu)
        return false;
    const u32 allowed =
        slot.affinity.load(std::memory_order_acquire) & slot.thread_affinity[index].load(std::memory_order_acquire);
    return (allowed & (1u << cpu)) != 0 || (cpu == 0 && (allowed & configured_cpu_mask()) == 0);
}

std::optional<SchedulerState::Picked> SchedulerState::pick(smp::CpuId cpu)
{
    if (shutdown_admission_closed_ || cpu >= smp::max_cpus || !cpu_online(cpu) || cpu_slots_[cpu].occupied())
        return std::nullopt;
    auto &own = run_queues_.get(cpu);
    // Own pops are bounded by the entries present now (plus one that a push
    // may add meanwhile); each other online queue is tried once.
    usize budget = own.size() + 1;
    u32 tried = 1u << cpu;
    for (;;)
    {
        std::optional<ThreadHandle> handle{};
        smp::CpuId source = cpu;
        if (budget != 0)
        {
            --budget;
            handle = own.pop();
            if (!handle) budget = 0;
        }
        if (!handle)
        {
            smp::CpuId victim = no_cpu;
            usize longest = 0;
            for (smp::CpuId other = 0; other < cpu_count_; ++other)
                if (cpu_online(other) && (tried & (1u << other)) == 0)
                    if (const auto length = run_queues_.get(other).size(); length > longest)
                    {
                        victim = other;
                        longest = length;
                    }
            if (victim == no_cpu) return std::nullopt;
            tried |= 1u << victim;
            source = victim;
            handle = run_queues_.get(victim).try_take_first(
                [&](ThreadHandle entry) { return steal_eligible(entry, victim, cpu); });
            if (!handle) continue;
        }
        if (!handle->valid() || handle->index >= max_thread_slots) continue;
        auto &slot = slots_[handle->index / max_threads_per_process];
        auto &thread = slot.pcb.threads_[handle->index % max_threads_per_process];
        smp::CpuId placed = no_cpu;
        std::optional<Picked> picked{};
        {
            TaskGuard guard{slot};
            // A reused slot holds another tid: the entry is stale.
            if (thread.slot_state == ThreadSlotState::free || thread.tid != handle->tid) continue;
            // An entry the thread no longer names is superseded; its field
            // belongs to the newer entry.
            if (thread.queued_cpu != source) continue;
            set_queued_locked(slot, thread, no_cpu);
            if (thread.on_cpu != no_cpu)
            {
                smp::count_check(smp::KernelCheck::queue, "queued thread is on a CPU");
                continue;
            }
            const auto &process = slot.pcb;
            if (thread.slot_state != ThreadSlotState::live || !may_queue(slot, thread) ||
                slot.state() != ProcessLifecycle::live || process.state_ == ProcessState::zombie ||
                process.state_ == ProcessState::exited)
                continue;
            // A thread with no native context yet (between creation and its
            // image) keeps its place behind the others, as does one the
            // affinity no longer admits here.
            if (!thread.native_active || !thread_admits(process, thread, cpu))
                placed = enqueue_locked(slot, thread, true);
            else
            {
                picked = Picked{{thread.owner, thread.tid}, thread.state == ProcessState::blocked};
                occupy_locked(cpu, slot, thread);
            }
        }
        notify_cpu(placed);
        if (picked)
        {
            if (source != cpu) smp::detail::owner_add(smp::evidence_cpus[cpu].steals);
            if (picked->probed) smp::detail::owner_add(smp::evidence_cpus[cpu].probes);
            return picked;
        }
    }
}

bool SchedulerState::resume_current(smp::CpuId cpu)
{
    if (cpu >= smp::max_cpus) return false;
    auto &current = cpu_slots_[cpu];
    if (!current.occupied() || !current.process || !current.thread || current.thread->tid != current.tid)
        return false;
    TaskHold hold{task_slot(*current.process)};
    if (current.thread->state == ProcessState::runnable) current.thread->state = ProcessState::running;
    return current.thread->state == ProcessState::running;
}

bool SchedulerState::probe_parked(ProcessControlBlock &process, ThreadControlBlock &thread)
{
    if (!in_table(process)) return false;
    auto &slot = task_slot(process);
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{slot};
        if (thread.state != ProcessState::blocked || !thread.wait_registration.active() || thread.on_cpu != no_cpu ||
            thread.queued_cpu != no_cpu || thread.probe)
            return false;
        // A stopped or frozen group keeps the probe without an entry: its
        // continue or unfreeze queues the thread (spec 4.9).
        thread.probe = true;
        placed = enqueue_locked(slot, thread);
    }
    notify_cpu(placed);
    return true;
}

void SchedulerState::settle_park(smp::CpuId cpu, WaitClass wait_class, bool probe)
{
    if (cpu >= smp::max_cpus) return;
    auto &current = cpu_slots_[cpu];
    if (!current.occupied() || !current.process || !current.thread || current.thread->tid != current.tid ||
        !in_table(*current.process))
        return;
    auto &thread = *current.thread;
    TaskHold hold{task_slot(*current.process)};
    if (thread.state != ProcessState::blocked || !thread.wait_registration.active()) return;
    thread.wait_class = wait_class;
    // The owner check at park: put_prev queues a probed thread.
    if (probe) thread.probe = true;
}

SchedulerState::WaitView SchedulerState::wait_view(const ProcessControlBlock &process, const ThreadControlBlock &thread)
{
    if (!in_table(process)) return WaitView::not_waiting;
    TaskHold hold{task_slot(process)};
    if (thread.state != ProcessState::blocked || !thread.wait_registration.active()) return WaitView::not_waiting;
    return thread.on_cpu != no_cpu ? WaitView::armed : WaitView::parked;
}

bool SchedulerState::probe_armed(ProcessControlBlock &process, ThreadControlBlock &thread)
{
    if (!in_table(process)) return false;
    smp::CpuId cpu = no_cpu;
    {
        TaskHold hold{task_slot(process)};
        if (thread.state != ProcessState::blocked || !thread.wait_registration.active() || thread.on_cpu == no_cpu)
            return false;
        thread.probe = true;
        cpu = thread.on_cpu;
    }
    if (cpu != smp::current_cpu() && cpu < smp::max_cpus) smp::send_ipi(cpu, smp::ipi_kick);
    return true;
}

SchedulerState::IdleCheck SchedulerState::enter_idle(smp::CpuId cpu)
{
    if (cpu >= smp::max_cpus) return IdleCheck::halt;
    idle_[cpu].store(true, std::memory_order_relaxed);
    if constexpr (smp::concurrent_locks)
    {
        // Idle side of the handshake, a store-buffering pair with
        // notify_cpu: after this fence either a pusher sees the flag and
        // wakes this CPU, or the loads below see its entry.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    if (run_queues_.get(cpu).size() != 0)
    {
        idle_[cpu].store(false, std::memory_order_relaxed);
        return IdleCheck::own_work;
    }
    if constexpr (smp::concurrent_locks)
        for (smp::CpuId other = 0; other < cpu_count_; ++other)
            if (other != cpu && cpu_online(other) && run_queues_.get(other).size() != 0) return IdleCheck::steal_hint;
    return IdleCheck::halt;
}

usize SchedulerState::audit_queues()
{
    usize repairs = 0;
    const TableHold table{task_table_};
    for (auto &slot : slots_)
    {
        if (slot.state() != ProcessLifecycle::live) continue;
        u32 placed = 0;
        {
            TaskGuard guard{slot};
            auto &process = slot.pcb;
            // A frozen group's threads stay unqueued on purpose (spec 4.8).
            if (process.execution() != ProcessExecution::user_process || slot.flagged(RunFlag::frozen) ||
                (process.job_.stopped && !slot.flagged(RunFlag::pending_exit)))
                continue;
            for (auto &thread : process.live_threads())
            {
                const auto index = process.slot_of(thread);
                if (index < max_threads_per_process &&
                    (slot.queued_cpu[index].load(std::memory_order_relaxed) != thread.queued_cpu ||
                     slot.on_cpu[index].load(std::memory_order_relaxed) != thread.on_cpu))
                    smp::count_check(smp::KernelCheck::queue, "placement mirror differs from its TCB");
                if (!thread.native_active || thread.on_cpu != no_cpu || thread.queued_cpu != no_cpu) continue;
                if (thread.state == ProcessState::blocked && !thread.wait_registration.active())
                {
                    // stuck_completed: the wait completed without a wake.
                    smp::count_check(smp::KernelCheck::audit_repairs, "audit: blocked thread with a completed wait");
                    thread.wait_reason = ThreadWaitReason::none;
                    thread.wait_key = 0;
                    thread.state = ProcessState::runnable;
                }
                else if (may_queue(slot, thread))
                    // orphan_runnable: may run, but nothing holds an entry for it.
                    smp::count_check(smp::KernelCheck::audit_repairs, "audit: runnable thread neither queued nor on a CPU");
                else
                    continue;
                ++repairs;
                if (const auto cpu = enqueue_locked(slot, thread); cpu != no_cpu) placed |= 1u << cpu;
            }
        }
        for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
            if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
    }
    return repairs;
}

Status SchedulerState::mark_on_cpu(ProcessId pid, ThreadId tid, smp::CpuId cpu)
{
    smp::check_exclusive("SchedulerState::mark_on_cpu");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (!cpu_online(cpu))
        return Status::invalid_argument("thread CPU is not online");
    auto *process = find(pid);
    auto *thread = find_thread(pid, tid);
    if (!process || !thread)
        return Status::not_found("thread not found");
    if (thread->on_cpu == cpu)
        return Status::success();
    if (thread->on_cpu != no_cpu)
        return Status::busy("thread is running on another CPU");
    if (cpu_slots_[cpu].occupied())
        return Status::busy("CPU already runs another thread");
    if (thread->state != ProcessState::runnable || process->state() == ProcessState::zombie ||
        process->state() == ProcessState::exited || process->job_.stopped || !thread_admits(*process, *thread, cpu) ||
        (in_table(*process) && task_slot(*process).flagged(RunFlag::frozen)))
        return Status::invalid_argument("thread cannot run on this CPU");
    occupy_cpu(cpu, *process, *thread);
    return Status::success();
}

Status SchedulerState::clear_on_cpu(ProcessId pid, ThreadId tid, smp::CpuId cpu)
{
    smp::check_exclusive("SchedulerState::clear_on_cpu");
    smp::check_drained(pid, "SchedulerState::clear_on_cpu");
    if (cpu >= cpu_count_)
        return Status::invalid_argument("thread CPU is not configured");
    auto *process = find(pid);
    auto *thread = find_thread(pid, tid);
    if (!process || !thread)
        return Status::not_found("thread not found");
    if (thread->on_cpu != cpu)
        return Status::invalid_argument("thread is not on this CPU");
    // put_prev for that CPU: a running thread becomes runnable and is queued.
    vacate_cpu_slot(cpu);
    return Status::success();
}

void SchedulerState::forget_queued(const ProcessControlBlock &process, ThreadControlBlock &thread)
{
    if (!in_table(process))
    {
        if (thread.queued_cpu < smp::max_cpus)
            static_cast<void>(run_queues_.get(thread.queued_cpu).remove(handle_of(process, thread)));
        thread.queued_cpu = no_cpu;
        return;
    }
    auto &slot = task_slot(process);
    TaskHold hold{slot};
    forget_queued_locked(slot, thread);
}

void SchedulerState::forget_queued_locked(ProcessSlot &slot, ThreadControlBlock &thread)
{
    if (thread.queued_cpu == no_cpu)
        return;
    if (thread.queued_cpu < smp::max_cpus)
        static_cast<void>(run_queues_.get(thread.queued_cpu).remove(handle_of(slot.pcb, thread)));
    set_queued_locked(slot, thread, no_cpu);
}

void SchedulerState::vacate_cpu_slot(usize cpu)
{
    auto &current = cpu_slots_[cpu];
    auto *process = current.process;
    auto *thread = current.thread;
    const auto tid = current.tid;
    current.pid = 0;
    current.tid = 0;
    current.process = nullptr;
    current.thread = nullptr;
    if (!process) return;
    if (!in_table(*process))
    {
        if (thread && thread->tid == tid && thread->on_cpu == cpu) thread->on_cpu = no_cpu;
        return;
    }
    auto &slot = task_slot(*process);
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{slot};
        if (thread && thread->tid == tid && thread->on_cpu == cpu)
        {
            if (thread->state == ProcessState::running) thread->state = ProcessState::runnable;
            placed = enqueue_locked(slot, *thread);
            // The last write to the thread: another CPU may pick it now.
            set_on_cpu_locked(slot, *thread, no_cpu);
        }
        // The CPU-slot pin taken by occupy; the last one of a reaped slot frees it.
        if (slot.pins == 0)
            smp::count_check(smp::KernelCheck::pin, "CPU slot pin underflow");
        else if (--slot.pins == 0 && slot.state() == ProcessLifecycle::reaped)
            ProcessPin::free_locked(slot);
    }
    notify_cpu(placed);
}

void SchedulerState::occupy_cpu(smp::CpuId cpu, ProcessControlBlock &process, ThreadControlBlock &thread)
{
    // Once secondary CPUs are admitted, a CPU fills only its own slot: a
    // section on another CPU reads its bound caller, never a remote write.
    if constexpr (smp::lock_debug_enabled)
        if (smp::exclusive_armed.load(std::memory_order_relaxed) && cpu != smp::current_cpu())
            smp::count_exclusive_violation("SchedulerState::occupy_cpu on another CPU");
    auto &current = cpu_slots_[cpu];
    if (current.occupied() && current.thread == &thread && current.tid == thread.tid && thread.on_cpu == cpu)
    {
        // Dispatched again in place: the slot keeps its pin.
        TaskHold hold{task_slot(process)};
        thread.state = ProcessState::running;
        ++thread.dispatch_count;
        ++thread.cpu_time_ticks;
        const bool account_cpu = process_counts_as_cpu_work(process);
        process.record_dispatch(cpu, account_cpu);
        ++current.dispatches;
        if (account_cpu) ++current.busy_dispatches;
        return;
    }
    if (current.occupied() || current.process) vacate_cpu_slot(cpu);
    if (!in_table(process)) return;
    auto &slot = task_slot(process);
    TaskHold hold{slot};
    occupy_locked(cpu, slot, thread);
}

void SchedulerState::occupy_locked(smp::CpuId cpu, ProcessSlot &slot, ThreadControlBlock &thread)
{
    auto &process = slot.pcb;
    auto &current = cpu_slots_[cpu];
    if (current.occupied())
        smp::count_check(smp::KernelCheck::pin, "occupied CPU slot dispatched again");
    forget_queued_locked(slot, thread);
    thread.probe = false;
    if (thread.state != ProcessState::blocked) thread.state = ProcessState::running;
    ++thread.dispatch_count;
    ++thread.cpu_time_ticks;
    thread.last_cpu = cpu;
    set_on_cpu_locked(slot, thread, cpu);
    const bool account_cpu = process_counts_as_cpu_work(process);
    process.record_dispatch(cpu, account_cpu);
    // The CPU-slot pin (spec 3.2 (b)): the slot outlives a reap until put_prev.
    ++slot.pins;
    current.pid = process.pid();
    current.tid = thread.tid;
    current.process = &process;
    current.thread = &thread;
    ++current.dispatches;
    if (account_cpu) ++current.busy_dispatches;
}

void SchedulerState::install_thread(ProcessControlBlock &process, ThreadControlBlock &slot_thread,
                                    const ThreadControlBlock &created)
{
    if (!in_table(process))
    {
        slot_thread = created;
        return;
    }
    auto &slot = task_slot(process);
    // A reused exited record's channel entries and deadline go first.
    release_wait_links(process, slot_thread);
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{slot};
        forget_wait_deadline_locked(process.thread_index(slot_thread));
        // A reused exited record may still name a stale queue entry.
        forget_queued_locked(slot, slot_thread);
        slot_thread = created;
        slot_thread.on_cpu = no_cpu;
        slot_thread.queued_cpu = no_cpu;
        set_on_cpu_locked(slot, slot_thread, no_cpu);
        set_queued_locked(slot, slot_thread, no_cpu);
        // A new thread starts with every CPU; clone then copies its caller's mask.
        store_thread_affinity(process, slot_thread, cpu_affinity_any);
        if (process.state_ == ProcessState::created) process.set_state(ProcessState::runnable);
        placed = enqueue_locked(slot, slot_thread);
    }
    notify_cpu(placed);
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
    if (process->live_count() == 0) return Status::invalid_argument("process has no live thread");
    u32 placed = 0;
    {
        auto &slot = task_slot(*process);
        TaskHold hold{slot};
        // Only the lifecycle is stored: created becomes live.
        process->set_state(ProcessState::runnable);
        for (auto &thread : process->live_threads())
        {
            thread.wait_registration.cancel();
            thread.native_deadline.cancel();
            forget_wait_deadline_locked(process->thread_index(thread));
            thread.wait_reason = ThreadWaitReason::none;
            thread.wait_key = 0;
            thread.probe = false;
            thread.state = ProcessState::runnable;
            if (thread.on_cpu == no_cpu)
                if (const auto cpu = enqueue_locked(slot, thread); cpu != no_cpu) placed |= 1u << cpu;
        }
    }
    for (const auto &thread : process->live_threads()) release_wait_links(*process, thread);
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
    return Status::success();
}

Status SchedulerState::block_process(ProcessId pid)
{
    auto *process = find(pid);
    if (!process)
    {
        return Status::not_found("process not found");
    }
    {
        auto &slot = task_slot(*process);
        TaskHold hold{slot};
        for (auto &thread : process->live_threads())
        {
            thread.state = ProcessState::blocked;
            thread.probe = false;
            forget_queued_locked(slot, thread);
        }
    }
    for (usize i = 0; i < cpu_slots_.size(); ++i)
    {
        if (cpu_slots_[i].pid == pid)
        {
            vacate_cpu_slot(i);
        }
    }
    return Status::success();
}

Result<ThreadId> SchedulerState::pick_next_thread(ProcessControlBlock &process, ThreadId current, smp::CpuId cpu)
{
    if (process.record_count() == 0)
    {
        return Status::not_found("process has no threads");
    }

    // Slot order; the search starts after the current thread's slot.
    constexpr usize count = max_threads_per_process;
    usize start = 0;
    for (usize i = 0; i < count; ++i)
    {
        if (process.thread_slot(i).slot_state != ThreadSlotState::free && process.thread_slot(i).tid == current)
        {
            start = (i + 1) % count;
            break;
        }
    }

    for (usize offset = 0; offset < count; ++offset)
    {
        auto &thread = process.thread_slot((start + offset) % count);
        if (thread.slot_state != ThreadSlotState::live || !thread_admits(process, thread, cpu))
            continue;
        if (thread.state == ProcessState::runnable || (thread.state == ProcessState::running && thread.tid == current &&
                                                       !thread_running_on_other_cpu(thread, cpu)))
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
    const auto previous_pid = cpu_slots_[cpu].pid;
    const auto previous_tid = cpu_slots_[cpu].tid;
    if (!next)
    {
        return next.status();
    }
    if (requested)
    {
        const auto *thread = find_thread(next.value(), requested);
        const auto *owner = find(next.value());
        if (!thread || !owner || !thread_admits(*owner, *thread, cpu) || thread_running_on_other_cpu(*thread, cpu) ||
            (thread->state != ProcessState::runnable &&
             !(thread->state == ProcessState::running && requested == previous_tid && next.value() == previous_pid)))
            return Status::invalid_argument("requested thread cannot be activated");
    }
    auto *selected = find(next.value());
    if (!selected)
    {
        return Status::not_found("selected process disappeared");
    }
    // The current thread stays running until the choice is made: pick_next_thread
    // admits it after every other runnable thread of its process. Occupying the
    // CPU with another thread puts the current one back (runnable).
    auto selected_thread = requested ? Result<ThreadId>(requested)
                                    : pick_next_thread(*selected, selected->pid() == previous_pid ? previous_tid : 0, cpu);
    if (!selected_thread)
    {
        return selected_thread.status();
    }
    auto *thread = find_thread(selected->pid(), selected_thread.value());
    if (!thread)
    {
        return Status::not_found("selected thread disappeared");
    }
    occupy_cpu(cpu, *selected, *thread);
    return selected->pid();
}

ProcessId SchedulerState::current_pid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return cpu_slots_[cpu].pid;
}

ThreadId SchedulerState::current_tid(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_)
    {
        return 0;
    }
    return cpu_slots_[cpu].tid;
}

ProcessControlBlock *SchedulerState::find(ProcessId pid)
{
    smp::check_exclusive("SchedulerState::find");
    auto *slot = visible_slot(pid);
    return slot ? &slot->pcb : nullptr;
}

const ProcessControlBlock *SchedulerState::find(ProcessId pid) const
{
    smp::check_exclusive("SchedulerState::find");
    const auto *slot = visible_slot(pid);
    return slot ? &slot->pcb : nullptr;
}

ProcessSlot *SchedulerState::visible_slot(ProcessId pid)
{
    for (auto &slot : slots_)
        if (slot.names(pid)) return &slot;
    return nullptr;
}

const ProcessSlot *SchedulerState::visible_slot(ProcessId pid) const
{
    for (const auto &slot : slots_)
        if (slot.names(pid)) return &slot;
    return nullptr;
}

std::optional<usize> SchedulerState::slot_of(ProcessId pid) const
{
    if (!pid) return std::nullopt;
    for (usize i = 0; i < slots_.size(); ++i)
        if (slots_[i].pid.load(std::memory_order_acquire) == pid) return i;
    return std::nullopt;
}

ProcessPin SchedulerState::pin(ProcessId pid)
{
    auto *slot = visible_slot(pid);
    return slot ? ProcessPin::acquire(*slot, pid) : ProcessPin{};
}

ProcessSlot *SchedulerState::claim_process_slot()
{
    const TableHold table{task_table_};
    return claim_free_slot(slots_.data(), slots_.size(), next_slot_);
}

void SchedulerState::release_process_slot(ProcessSlot &slot)
{
    const TableHold table{task_table_};
    release_slot(slot);
}

void SchedulerState::free_thread_slot(ProcessControlBlock &process, ThreadControlBlock &thread)
{
    forget_queued(process, thread);
    store_thread_affinity(process, thread, cpu_affinity_any);
    if (!in_table(process))
    {
        thread = ThreadControlBlock{};
        return;
    }
    // A free slot keeps no channel entry and no deadline (spec 3.3).
    release_wait_links(process, thread);
    // The placement mirrors follow the reset (exec frees its caller while
    // the caller's CPU slot still names it; that put_prev then skips it).
    auto &slot = task_slot(process);
    TaskHold hold{slot};
    forget_wait_deadline_locked(process.thread_index(thread));
    thread = ThreadControlBlock{};
    set_on_cpu_locked(slot, thread, no_cpu);
    set_queued_locked(slot, thread, no_cpu);
}

u16 SchedulerState::thread_affinity(const ProcessControlBlock &process, const ThreadControlBlock &thread) const
{
    const auto index = process.thread_index(thread);
    if (index == invalid_thread_index || &slots_[index / max_threads_per_process].pcb != &process)
        return cpu_affinity_any;
    return static_cast<u16>(
        slots_[index / max_threads_per_process].thread_affinity[index % max_threads_per_process].load(
            std::memory_order_relaxed));
}

void SchedulerState::store_thread_affinity(const ProcessControlBlock &process, const ThreadControlBlock &thread,
                                           u16 mask)
{
    const auto index = process.thread_index(thread);
    if (index == invalid_thread_index || &slots_[index / max_threads_per_process].pcb != &process) return;
    auto &slot = slots_[index / max_threads_per_process];
    slot.thread_affinity[index % max_threads_per_process].store(mask, std::memory_order_relaxed);
    // The process mask's mirror follows too: exec sets the PCB mask directly.
    slot.affinity.store(process.cpu_affinity_mask(), std::memory_order_release);
}

ThreadControlBlock *SchedulerState::thread_at(ThreadHandle handle)
{
    smp::check_exclusive("SchedulerState::thread_at");
    if (!handle.valid() || handle.index >= max_thread_slots) return nullptr;
    auto &slot = slots_[handle.index / max_threads_per_process];
    auto &thread = slot.pcb.threads_[handle.index % max_threads_per_process];
    return slot.state() != ProcessLifecycle::free && thread.slot_state != ThreadSlotState::free &&
                   thread.tid == handle.tid
               ? &thread
               : nullptr;
}

const ThreadControlBlock *SchedulerState::thread_at(ThreadHandle handle) const
{
    return const_cast<SchedulerState *>(this)->thread_at(handle);
}

bool SchedulerState::queued_on(smp::CpuId cpu, ThreadRef ref) const
{
    const auto *process = find(ref.pid);
    if (!process || cpu >= smp::max_cpus) return false;
    for (const auto &thread : process->thread_records())
        if (thread.tid == ref.tid) return run_queues_.get(cpu).contains(handle_of(*process, thread));
    return false;
}

usize SchedulerState::background_process_count() const
{
    usize count = 0;
    for (const auto &process : process_range())
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
    const auto &slot = cpu_slots_[cpu];
    return {slot.dispatches, slot.busy_dispatches, slot.pid, slot.tid};
}

u64 SchedulerState::total_dispatches() const
{
    u64 total = 0;
    for (usize i = 0; i < cpu_count_; ++i)
    {
        total += cpu_slots_[i].dispatches;
    }
    return total;
}

u64 SchedulerState::total_accounted_dispatches() const
{
    u64 total = 0;
    for (usize i = 0; i < cpu_count_; ++i)
    {
        total += cpu_slots_[i].busy_dispatches;
    }
    return total;
}

u8 SchedulerState::cpu_usage_percent(smp::CpuId cpu) const
{
    if (cpu >= cpu_count_ || cpu_slots_[cpu].dispatches == 0)
    {
        return 0;
    }
    return static_cast<u8>((cpu_slots_[cpu].busy_dispatches * 100u) / cpu_slots_[cpu].dispatches);
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
    reset_console_terminal();
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
DescriptionPositionLock *SchedulerState::position_lock(const OpenFileDescription *description)
{
    const auto base = reinterpret_cast<uptr>(descriptions_.data());
    const auto address = reinterpret_cast<uptr>(description);
    if (address < base || address - base >= sizeof(descriptions_) || (address - base) % sizeof(OpenFileDescription))
        return nullptr;
    return &positions_[(address - base) / sizeof(OpenFileDescription)];
}
ProcessId SchedulerState::resources_owner(const ProcessResources &resources) const
{
    for (const auto &process : process_range())
        if (&process.resources() == &resources)
            return process.pid();
    return 0;
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
    for (auto &process : process_range())
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
    smp::check_drained(pid, "SchedulerState::clear_current");
    for (usize cpu = 0; cpu < cpu_slots_.size(); ++cpu)
        if (cpu_slots_[cpu].pid == pid)
            vacate_cpu_slot(cpu);
}
Status SchedulerState::activate(ProcessId pid, smp::CpuId cpu)
{
    if (cpu >= cpu_count_)
        return Status::invalid_argument("invalid activation CPU");
    if (pid == 0)
    {
        put_prev(cpu);
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
        !process->can_run_on(cpu) || process->job_.stopped)
        return Status::invalid_argument("thread owner cannot be activated");
    if (const auto *thread = find_thread(pid, tid); thread && !thread_admits(*process, *thread, cpu))
        return Status::invalid_argument("thread affinity excludes this CPU");
    auto selected = dispatch_selected(cpu, pid, tid);
    return selected ? Status::success() : selected.status();
}
Result<ProcessId> SchedulerState::fork_process(ProcessId parent, bool publish)
{
    smp::check_exclusive("SchedulerState::fork_process");
    smp::check_drained(parent, "SchedulerState::fork_process");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *source = find(parent);
    if (source == nullptr)
        return Status::not_found("parent process not found");
    if (source->resources().unix_receive_operations || source->resources().prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation cannot be cloned by fork");
    if (source->state() == ProcessState::zombie || source->state() == ProcessState::exited ||
        source->record_count() == 0)
        return Status::invalid_argument("parent process is not alive");
    if (source->live_count() == 0) return Status::invalid_argument("cannot fork a retired thread group");
    // Fork capacity is a retryable process-resource limit (EAGAIN), not an
    // arithmetic overflow. Check before retaining cwd or descriptor ownership.
    if (source->children().full() || process_range().size() == max_processes)
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
    const ThreadControlBlock *calling_thread = &source->leader();
    for (const auto &thread : source->thread_records())
        if (thread.tid == current_tid(smp::current_cpu()) && current_pid() == parent)
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
    {
        // The parent's table under its files lock, the duplicated references
        // under ofd_table (files -> ofd_table); the child is not visible yet.
        const FilesLock::Lock::Guard files(source->resources().files_lock.lock());
        const OfdTableLock::Guard table(ofd_table_);
        child_resources.files = parent_resources.files;
        for (const auto &slot : child_resources.files.entries())
            if (slot.used)
                if (auto *description = open_description(slot.description))
                    ++description->references;
    }
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
    child_resources.memory_limits = parent_resources.memory_limits;
    child->set_parent(parent);
    child->group_ = source->group_;
    child->session_ = source->session_;
    child->has_execed_ = false;
    child->console_epoch_ = console_input_.inherit(source->console_epoch_);
    child->ctty_detached_ = source->ctty_detached_;
    child->set_credentials(source->credentials());
    child->set_execution(source->execution());
    child->set_address_space_id(next_address_space_id_++);
    child->set_background(source->background());
    child->set_priority(source->priority());
    child->set_cpu_affinity_mask(source->cpu_affinity_mask());
    task_slot(*child).affinity.store(source->cpu_affinity_mask(), std::memory_order_release);
    auto &child_thread = child->leader();
    child_thread.priority = calling_thread->priority;
    child_thread.tls = calling_thread->tls;
    child_thread.native_signal_mask = calling_thread->native_signal_mask;
    store_thread_affinity(*child, child_thread, thread_affinity(*source, *calling_thread));
    child_thread.native_frame = calling_thread->native_frame;
    child_thread.native_active = false;
    child_thread.syscall_pending = false;
    child_thread.native_deadline = {};
    child_thread.tls.clear_tid_address = 0;
    child_thread.tls.set_tid_address = 0;
    child_thread.tls.robust_list = 0;
    child_thread.tls.robust_list_length = 0;
    child_thread.wait_reason = ThreadWaitReason::none;
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
        for (const auto &member : process_range())
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
    for (const auto &member : process_range())
        if (member.process_group_id() == caller)
            return Status::permission("calling process identity already names a process group");
    process->session_.id = caller;
    process->group_.id = caller;
    process->ctty_detached_ = false;
    if (console_input_.inherit(process->console_epoch_)) cancel_console_epoch(process->console_epoch_);
    return caller;
}
Status SchedulerState::preflight_replace_image(ProcessId pid, const arch::CpuContext &context) const
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (auto status = arch::validate_architecture(context.architecture); !status.ok())
        return status;
    const auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->resources().unix_receive_operations || process->resources().prepared_descriptor_operations)
        return Status::busy("prepared UNIX receive must finish before exec");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::invalid_argument("cannot exec an exited process");
    if (process->record_count() == 0)
        return Status::invalid_argument("process has no thread");
    if (process->live_count() == 0) return Status::invalid_argument("cannot exec a retired thread group");
    return Status::success();
}
Status SchedulerState::replace_image(ProcessId pid, arch::CpuContext context, const MemoryMap &memory)
{
    smp::check_exclusive("SchedulerState::replace_image");
    smp::check_drained(pid, "SchedulerState::replace_image");
    if (auto status = preflight_replace_image(pid, context); !status) return status;
    auto *process = find(pid);
    for (const auto &thread : process->thread_records())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("entry stack references must be escrowed before exec");
    if (native_tcp_service_) native_tcp_service_->cancel_process(pid);
    // Cleared under the files lock a batch at a time, released after it (spec 4.12).
    static_cast<void>(close_on_exec_descriptors(process->resources()));
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
    // Siblings hold nothing any more (their leases are escrowed): their
    // slots are cleared in place and the leader's slot carries the image.
    for (usize index = 1; index < max_threads_per_process; ++index)
        if (process->thread_slot(index).slot_state != ThreadSlotState::free)
            free_thread_slot(*process, process->thread_slot(index));
    auto &thread = process->leader();
    thread.context = context;
    thread.tls = {};
    thread.native_deadline.cancel();
    thread.wait_registration.cancel();
    thread.console_wait_epoch = 0;
    thread.wait_reason = ThreadWaitReason::none;
    thread.wait_key = 0;
    thread.probe = false;
    thread.state = ProcessState::runnable;
    thread.slot_state = ThreadSlotState::live;
    process->set_execution(context.mode == arch::PrivilegeMode::user ? ProcessExecution::user_process
                                                                     : ProcessExecution::kernel_thread);
    process->set_address_space_id(next_address_space_id_++);
    process->has_execed_ = true;
    // A leader that was parked or had exited while a sibling called exec is
    // on no CPU: nothing else queues it. On its CPU, put_prev below does.
    smp::CpuId placed = no_cpu;
    if (in_table(*process))
    {
        release_wait_links(*process, thread);
        auto &slot = task_slot(*process);
        TaskHold hold{slot};
        forget_wait_deadline_locked(process->thread_index(thread));
        // The exec claim ends with its commit: no sibling is left to thaw.
        if (slot.transition == GroupTransition::exec)
        {
            slot.transition = GroupTransition::none;
            slot.transition_tid = 0;
            slot.set_flag(RunFlag::frozen, false);
        }
        if (thread.on_cpu == no_cpu) placed = enqueue_locked(slot, thread);
    }
    notify_cpu(placed);
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
    smp::check_exclusive("SchedulerState::finish_process");
    smp::check_drained(pid, "SchedulerState::finish_process");
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->resources().unix_receive_operations || process->resources().prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation must finish before exit");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::already_exists("process has already exited");
    for (const auto &thread : process->thread_records())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("thread entry stacks must be retired before process exit");
    // Adoption is validated before the completion becomes observable.
    if (auto status = reparent_children_on_exit(pid); !status) return status;
    // The terminal hangup is queued before the lease retires (its signals are
    // drained before the orphan pass of the exit commit, as in Linux do_exit).
    hangup_exiting_leader(*process);
    retire_console_process(pid);
    if (native_tcp_service_) native_tcp_service_->cancel_process(pid);
    process->set_exit_code(code);
    process->set_termination_signal(signal);
    if (auto *slot = visible_slot(pid))
    {
        TaskGuard guard{*slot};
        process->set_state(ProcessState::zombie);
        process->job_ = {};
        slot->set_flag(RunFlag::stopped, false);
        for (auto &thread : process->thread_records())
        {
            thread.native_deadline.cancel();
            thread.wait_registration.cancel();
            forget_wait_deadline_locked(process->thread_index(thread));
            thread.syscall_pending = false;
            thread.wait_reason = ThreadWaitReason::none;
            thread.wait_key = 0;
            thread.probe = false;
            thread.state = ProcessState::exited;
            thread.slot_state = ThreadSlotState::exited;
            forget_queued_locked(*slot, thread);
        }
        slot->lifecycle.store(static_cast<u32>(ProcessLifecycle::zombie), std::memory_order_release);
    }
    // A zombie keeps no channel entry and no timer deadline.
    for (const auto &thread : process->thread_records()) release_wait_links(*process, thread);
    if (const auto index = process->process_slot(); index < max_processes)
        set_process_deadline(index, DeadlineHeap<deadline_keys>::never);
    release_resources(process->resources());
    clear_current(pid);
    wake_waiters(ThreadWaitReason::child, pid);
    wake_waiters(ThreadWaitReason::child, 0);
    wake_waiters(ThreadWaitReason::child, child_group_wait_key(process->process_group_id()));
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
    smp::check_exclusive("SchedulerState::reap_process");
    // A dead init is a terminal boot condition; PID 1 is never recycled.
    if (pid == user_init_pid && find(pid))
        return Status::busy("init must remain available for recovery diagnosis");
    auto *process = find(pid);
    if (process == nullptr)
        return Status::not_found("process not found");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        return Status::invalid_argument("process is not a zombie");
    for (const auto &thread : process->thread_records())
        if (thread.entry_stack.pool || thread.kernel_stack_top)
            return Status::busy("thread entry stacks must be retired before reaping");
    if (process->resources().native_space.root != 0 || !process->resources().native_pages.empty())
        return Status::busy("native address space must be released before reaping");
    // Nothing moves: other processes' PCBs, a kernel section's bound caller
    // and pinned slots stay where they are, so no drain is needed.
    retire_console_process(pid);
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
    if (auto status = user::native::private_signal_before_process_reap(*this); !status)
        return status;
#endif
    const auto parent = process->parent_pid();
    if (auto *owner = find(parent))
        static_cast<void>(owner->remove_child(pid));
    static_cast<void>(reparent_children(pid, 0));
    release_resources(process->resources());
    clear_current(pid);
    for (auto &thread : process->thread_records())
        forget_queued(*process, thread);
    auto *slot = visible_slot(pid);
    if (!slot) return Status::not_found("process not found");
    // The process released its external resources. Its slot is freed now,
    // or by the last unpin; the PCB stays in place until reallocation.
    release_process_slot(*slot);
    return Status::success();
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
    for (const auto &other : process_range())
        if (other.pid() != parent)
            for (const auto id : other.children())
                if (id == child)
                    return Status::fault("completed child is linked to another parent");
    if (process->state() != ProcessState::zombie && process->state() != ProcessState::exited)
        return Status::invalid_argument("child completion has not been published");
    if (!process->children().empty())
        return Status::busy("completed child descendants must be reparented first");
    for (const auto &thread : process->thread_records())
        if (thread.state != ProcessState::exited || thread.entry_stack.pool || thread.kernel_stack_top ||
            thread.native_active || thread.syscall_pending || thread.native_deadline.armed() ||
            thread.wait_registration.active() || thread.wait_reason != ThreadWaitReason::none || thread.wait_key)
            return Status::busy("completed child threads must be retired first");
    for (smp::CpuId cpu = 0; cpu < cpu_count_; ++cpu)
        if (cpu_slots_[cpu].pid == child)
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
    for (auto &slot : slots_)
    {
        if (!slot.visible()) continue;
        const auto &process = slot.pcb;
        if (process.pid() == user_init_pid || process.parent_pid() != 0 ||
            (process.state() != ProcessState::zombie && process.state() != ProcessState::exited))
            continue;
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
    for (auto &slot : slots_)
    {
        if (!slot.visible()) continue;
        const auto &process = slot.pcb;
        if (process.pid() != user_init_pid && process.parent_pid() == 0 &&
            (process.state() == ProcessState::zombie || process.state() == ProcessState::exited) &&
            reap_process(process.pid()).ok())
            ++reaped;
    }
    return reaped;
}
Status SchedulerState::block_current(ThreadWaitReason reason, u64 key)
{
    auto registration = register_current_wait(reason, key);
    return registration ? Status::success() : registration.status();
}
ThreadControlBlock *SchedulerState::find_thread(ProcessId pid, ThreadId tid)
{
    smp::check_exclusive("SchedulerState::find_thread");
    if (auto *process = find(pid))
        for (auto &thread : process->thread_records())
            if (thread.tid == tid)
                return &thread;
    return nullptr;
}
ThreadControlBlock *SchedulerState::current_thread()
{
    smp::check_exclusive("SchedulerState::current_thread");
    const auto cpu = smp::current_cpu();
    return find_thread(current_pid(cpu), current_tid(cpu));
}
Result<WaitTicket> SchedulerState::register_current_wait(ThreadWaitReason reason, u64 key)
{
    smp::check_exclusive("SchedulerState::register_current_wait");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (reason == ThreadWaitReason::none)
        return Status::invalid_argument("wait requires a reason");
    // The caller is the thread this CPU runs (its CPU slot names it).
    const auto cpu = smp::current_cpu();
    if (cpu >= smp::max_cpus) return Status::not_found("no current process to block");
    auto &running = cpu_slots_[cpu];
    auto *process = running.process;
    if (!running.occupied() || process == nullptr || process->pid() != running.pid || !in_table(*process))
        return Status::not_found("no current process to block");
    auto *current = running.thread;
    if (current == nullptr || current->tid != running.tid || current->owner != process->pid())
        return Status::not_found("no current thread to block");
    // Owner unlink: entries the previous wait left go before this one links.
    release_wait_links(*process, *current);
    WaitTicket armed{};
    {
        // Armed under the task lock (spec 4.2): a completion either finds the
        // wait registered or has not started.
        TaskHold hold{task_slot(*process)};
        if (current->state != ProcessState::running)
            return Status::busy("wait owner is not running");
        auto ticket = current->wait_registration.arm(process->pid(), current->tid, process->thread_index(*current));
        if (!ticket)
            return ticket.status();
        armed = ticket.value();
        current->state = ProcessState::blocked;
        current->wait_reason = reason;
        current->wait_key = key;
        current->probe = false;
        current->wait_class = WaitClass::unknown;
    }
    // Spec 4.3 A1: linked on its channel after the arm, before the caller
    // releases the lock of the condition it checked.
    if (channel_reason(reason)) static_cast<void>(link_wait(armed, reason, key));
    return armed;
}

bool SchedulerState::link_wait(WaitTicket ticket, ThreadWaitReason reason, u64 key)
{
    if (ticket.index >= max_thread_slots) return false;
    auto &links = wait_links_[ticket.index];
    u16 entry = WaitChannels::none;
    if (links.count < max_wait_links)
        entry = wait_channels_.insert(static_cast<u8>(reason), key,
                                      {static_cast<u32>(ticket.thread), static_cast<u32>(ticket.generation), ticket.index},
                                      links.head);
    if (entry != WaitChannels::none)
    {
        auto &slot = slots_[ticket.index / max_threads_per_process];
        TaskHold hold{slot};
        const auto &thread = slot.pcb.threads_[ticket.index % max_threads_per_process];
        if (thread.wait_registration.pending(ticket) && links.count < max_wait_links)
        {
            // The entry chains to the previous head (the owner's alone).
            links.head = entry;
            ++links.count;
            return true;
        }
    }
    else
    {
        // No entry: the wait cannot be woken through its channel. It
        // completes ready within a tick and its owner re-evaluates it. A wait
        // that is no longer pending needs nothing.
        u64 now{};
        {
            const DeadlineLock::Guard guard{deadline_lock_};
            now = deadline_clock_;
        }
        if (!arm_wait_deadline(ticket, now + wait_retry_ns, DeadlineAction::retry)) return true;
        unqueued_waits_.store(unqueued_waits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        const auto cpu = smp::current_cpu();
        if (cpu < smp::max_cpus) smp::detail::owner_add(smp::evidence_cpus[cpu].unqueued);
        return false;
    }
    // The wait completed meanwhile (a waker may have taken the entry
    // already): nothing is left to watch, so the entry goes now.
    static_cast<void>(wait_channels_.release(entry));
    return true;
}

bool SchedulerState::watch_channel(WaitTicket ticket, ThreadWaitReason reason, u64 key)
{
    smp::check_exclusive("SchedulerState::watch_channel");
    if (ticket.index >= max_thread_slots) return false;
    const auto cpu = smp::current_cpu();
    if (cpu >= smp::max_cpus || cpu_slots_[cpu].thread == nullptr || cpu_slots_[cpu].tid != ticket.thread)
        return false;
    return link_wait(ticket, reason, key);
}

void SchedulerState::release_wait_links(u16 index)
{
    if (index >= max_thread_slots) return;
    auto &links = wait_links_[index];
    WaitLinks taken{};
    {
        auto &slot = slots_[index / max_threads_per_process];
        TaskHold hold{slot};
        taken = links;
        links = {};
    }
    // Each entry is detached if no waker took it, then freed.
    u16 next = taken.head;
    for (usize i = 0; i < taken.count && next != WaitChannels::none; ++i)
    {
        const u16 entry = next;
        static_cast<void>(wait_channels_.release(entry, next));
    }
}

std::optional<WaitTicket> SchedulerState::pending_ticket(u16 index, u32 thread, u32 generation)
{
    if (index >= max_thread_slots) return std::nullopt;
    auto &slot = slots_[index / max_threads_per_process];
    TaskHold hold{slot};
    const auto &tcb = slot.pcb.threads_[index % max_threads_per_process];
    const auto ticket = tcb.wait_registration.ticket();
    if (tcb.slot_state == ThreadSlotState::free || static_cast<u32>(tcb.tid) != thread ||
        !tcb.wait_registration.active() || static_cast<u32>(ticket.generation) != generation)
        return std::nullopt;
    return ticket;
}

void SchedulerState::release_wait_links(const ProcessControlBlock &process, const ThreadControlBlock &thread)
{
    if (in_table(process)) release_wait_links(process.thread_index(thread));
}

void SchedulerState::settle_wait_links(smp::CpuId cpu)
{
    if (cpu >= smp::max_cpus) return;
    const auto &current = cpu_slots_[cpu];
    if (!current.occupied() || !current.process || !current.thread || !in_table(*current.process)) return;
    const auto index = current.process->thread_index(*current.thread);
    if (index >= max_thread_slots || wait_links_[index].count == 0) return;
    {
        TaskHold hold{task_slot(*current.process)};
        if (current.thread->wait_registration.active()) return;
    }
    release_wait_links(index);
}

void SchedulerState::publish_earliest_locked()
{
    earliest_.store(deadlines_.earliest(), std::memory_order_release);
}

void SchedulerState::forget_wait_deadline_locked(u16 index)
{
    if (index >= deadline_thread_keys) return;
    const DeadlineLock::Guard guard{deadline_lock_};
    if (deadlines_.remove(index))
    {
        deadline_waits_[index] = {};
        publish_earliest_locked();
    }
}

bool SchedulerState::arm_wait_deadline(WaitTicket ticket, u64 deadline, DeadlineAction action)
{
    if (ticket.index >= deadline_thread_keys) return false;
    auto &slot = slots_[ticket.index / max_threads_per_process];
    TaskHold hold{slot};
    const auto &thread = slot.pcb.threads_[ticket.index % max_threads_per_process];
    if (!thread.wait_registration.pending(ticket)) return false;
    const DeadlineLock::Guard guard{deadline_lock_};
    auto &entry = deadline_waits_[ticket.index];
    // One wait, one deadline: the earlier of two arms of the same wait.
    const DeadlineEntry armed{static_cast<u32>(ticket.thread), static_cast<u32>(ticket.generation), action};
    if (deadlines_.contains(ticket.index) && entry.thread == armed.thread && entry.generation == armed.generation &&
        deadlines_.when(ticket.index) <= deadline)
        return true;
    entry = armed;
    deadlines_.set(ticket.index, deadline);
    publish_earliest_locked();
    return true;
}

void SchedulerState::set_process_deadline(usize process_slot, u64 deadline)
{
    if (process_slot >= max_processes) return;
    const auto key = static_cast<u16>(deadline_thread_keys + process_slot);
    const DeadlineLock::Guard guard{deadline_lock_};
    if (deadline == DeadlineHeap<deadline_keys>::never) deadlines_.remove(key);
    else deadlines_.set(key, deadline);
    publish_earliest_locked();
}

u64 SchedulerState::deadline_of(u16 key)
{
    const DeadlineLock::Guard guard{deadline_lock_};
    return deadlines_.when(key);
}

usize SchedulerState::expire_deadlines(u64 now, std::span<u16, max_processes> processes)
{
    // Every process key fits in `processes`: a key popped here is set again
    // only by the caller's service, after this call.
    static_assert(deadline_keys - deadline_thread_keys == max_processes);
    usize written = 0;
    // Bounded batches: pop under the lock, act after releasing it.
    for (usize pass = 0; pass < 8; ++pass)
    {
        struct Due
        {
            u16 key{};
            DeadlineEntry entry{};
        };
        std::array<Due, 32> due{};
        usize count = 0;
        u64 late = 0;
        {
            const DeadlineLock::Guard guard{deadline_lock_};
            if (now > deadline_clock_) deadline_clock_ = now;
            while (count < due.size())
            {
                if (deadlines_.earliest() > now) break;
                const auto when = deadlines_.earliest();
                const auto key = deadlines_.pop_due(now);
                if (!key) break;
                // A process key at 0 is due on every step (a timer awaiting
                // its signal's dequeue or the wall clock): it was not late.
                if (when != 0 && now - when > late) late = now - when;
                due[count].key = *key;
                if (*key < deadline_thread_keys)
                {
                    due[count].entry = deadline_waits_[*key];
                    deadline_waits_[*key] = {};
                }
                ++count;
            }
            publish_earliest_locked();
        }
        if (const auto cpu = smp::current_cpu(); cpu < smp::max_cpus && late / 1000 > 0)
        {
            auto &record = smp::evidence_cpus[cpu].deadline_late_max_us;
            if (late / 1000 > record.load(std::memory_order_relaxed))
                record.store(static_cast<uptr>(late / 1000), std::memory_order_relaxed);
        }
        for (usize i = 0; i < count; ++i)
        {
            const auto &item = due[i];
            if (item.key >= deadline_thread_keys)
            {
                processes[written++] = static_cast<u16>(item.key - deadline_thread_keys);
                continue;
            }
            // Only the wait the key was armed for: one that completed or
            // re-armed meanwhile does not match.
            const auto ticket = pending_ticket(item.key, item.entry.thread, item.entry.generation);
            if (!ticket) continue;
            switch (item.entry.action)
            {
            case DeadlineAction::timeout:
                static_cast<void>(complete_wait(*ticket, WaitCompletion::timeout));
                break;
            case DeadlineAction::retry:
                static_cast<void>(complete_wait(*ticket, WaitCompletion::ready, WakeSource::timer));
                break;
            case DeadlineAction::probe:
                static_cast<void>(probe_wait(*ticket));
                break;
            }
        }
        if (count < due.size()) break;
    }
    return written;
}

bool SchedulerState::probe_wait(WaitTicket ticket)
{
    if (ticket.index >= max_thread_slots) return false;
    auto &slot = slots_[ticket.index / max_threads_per_process];
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{slot};
        auto &thread = slot.pcb.threads_[ticket.index % max_threads_per_process];
        if (thread.tid != ticket.thread || thread.state != ProcessState::blocked ||
            !thread.wait_registration.pending(ticket) || thread.probe)
            return false;
        thread.probe = true;
        // A parked waiter is queued now; an armed one by its put_prev.
        if (thread.on_cpu == no_cpu) placed = enqueue_locked(slot, thread);
    }
    notify_cpu(placed);
    return true;
}
bool SchedulerState::complete_wait(WaitTicket ticket, WaitCompletion completion, WakeSource source)
{
    u32 kicks = 0;
    const bool completed = complete_wait(ticket, completion, source, kicks);
    notify_cpus(kicks);
    return completed;
}
void SchedulerState::notify_cpus(u32 kicks)
{
    for (smp::CpuId cpu = 0; kicks != 0 && cpu < smp::max_cpus; ++cpu, kicks >>= 1)
        if ((kicks & 1U) != 0) notify_cpu(cpu);
}
bool SchedulerState::wait_pending(WaitTicket ticket)
{
    if (ticket.index >= max_thread_slots) return false;
    auto &slot = slots_[ticket.index / max_threads_per_process];
    TaskHold hold{slot};
    const auto &thread = slot.pcb.threads_[ticket.index % max_threads_per_process];
    return thread.slot_state != ThreadSlotState::free && thread.tid == ticket.thread &&
           thread.owner == ticket.owner && thread.wait_registration.pending(ticket);
}
bool SchedulerState::complete_wait(WaitTicket ticket, WaitCompletion completion, WakeSource source, u32 &kicks)
{
    smp::check_exclusive("SchedulerState::complete_wait");
    // O(1): the ticket names the waiter's slot; a reused slot holds another
    // tid. A registration armed outside register_current_wait has no slot.
    ProcessSlot *slot = nullptr;
    ThreadControlBlock *thread = nullptr;
    if (ticket.index != invalid_thread_index)
    {
        if (ticket.index >= max_thread_slots) return false;
        slot = &slots_[ticket.index / max_threads_per_process];
        thread = &slot->pcb.threads_[ticket.index % max_threads_per_process];
    }
    else if (auto *process = find(ticket.owner); process && in_table(*process))
    {
        slot = &task_slot(*process);
        thread = find_thread(ticket.owner, ticket.thread);
    }
    if (!slot || !thread) return false;
    smp::CpuId placed = no_cpu;
    {
        TaskHold hold{*slot};
        const auto &process = slot->pcb;
        if (thread->slot_state == ThreadSlotState::free || thread->tid != ticket.thread ||
            thread->owner != ticket.owner || process.pid() != ticket.owner || !lifecycle_visible(slot->state()) ||
            process.state_ == ProcessState::zombie || process.state_ == ProcessState::exited ||
            thread->state != ProcessState::blocked || !thread->wait_registration.complete(ticket, completion, source))
            return false;
        if (completion == WaitCompletion::ready || completion == WaitCompletion::timeout)
            count_wake(thread->wait_registration.source());
        // The wait's deadline goes with it (task -> deadline). A sleep's own
        // record (native_deadline) is the owner's: its resume settles it.
        forget_wait_deadline_locked(process.thread_index(*thread));
        thread->wait_reason = ThreadWaitReason::none;
        thread->wait_key = 0;
        thread->probe = false;
        thread->state = ProcessState::runnable;
        // A parked waiter is queued now; an armed one by its put_prev.
        if (thread->on_cpu == no_cpu) placed = enqueue_locked(*slot, *thread);
    }
    if (placed < smp::max_cpus) kicks |= u32{1} << placed;
    return true;
}

void SchedulerState::count_wake(WakeSource source)
{
    const auto cpu = smp::current_cpu();
    if (cpu < smp::max_cpus) smp::detail::owner_add(wakes_[cpu][static_cast<usize>(source)]);
}

u64 SchedulerState::wakes(WakeSource source) const
{
    u64 total = 0;
    for (const auto &cpu : wakes_) total += cpu[static_cast<usize>(source)].load(std::memory_order_relaxed);
    return total;
}
void SchedulerState::wake_waiters(ThreadWaitReason reason, u64 key, WakeSource source)
{
    smp::check_exclusive("SchedulerState::wake_waiters");
    std::array<WaitHashTicket, WaitChannels::wake_batch> batch{};
    for (;;)
    {
        // Detached under the bucket lock, completed after releasing it
        // (bucket -> task never nests). A wait that completed or re-armed
        // since it linked the entry does not match.
        const auto taken = wait_channels_.take(static_cast<u8>(reason), key, batch);
        for (usize i = 0; i < taken.count; ++i)
            if (const auto ticket = pending_ticket(batch[i].index, batch[i].thread, batch[i].generation))
                static_cast<void>(complete_wait(*ticket, WaitCompletion::ready, source));
        if (!taken.more) break;
    }
}

usize SchedulerState::probe_poll_waiters()
{
    usize probed = 0;
    const TableHold table{task_table_};
    for (auto &slot : slots_)
    {
        if (slot.state() != ProcessLifecycle::live) continue;
        u32 placed = 0;
        {
            TaskGuard guard{slot};
            auto &process = slot.pcb;
            if (process.execution() != ProcessExecution::user_process || process.job_.stopped ||
                slot.flagged(RunFlag::frozen))
                continue;
            for (auto &thread : process.live_threads())
            {
                if (!thread.native_active || thread.state != ProcessState::blocked || thread.probe ||
                    thread.wait_reason != ThreadWaitReason::poll || !thread.wait_registration.active() ||
                    thread.on_cpu != no_cpu || thread.queued_cpu != no_cpu)
                    continue;
                thread.probe = true;
                if (const auto cpu = enqueue_locked(slot, thread); cpu != no_cpu)
                {
                    placed |= 1u << cpu;
                    ++probed;
                }
                else
                    thread.probe = false;
            }
        }
        for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
            if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
    }
    return probed;
}

#include "console_input.inc"
#include "job_control.inc"
#include "group_transition.inc"
Status SchedulerState::close_shutdown_admission()
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission was already closed");
    if (active_unix_receives_ || active_descriptor_reservations_ || resource_drain_report_.active)
        return Status::busy("prepared resource operation prevents shutdown admission closure");
    shutdown_admission_closed_ = true;
    return {};
}

} // namespace ok::sched
