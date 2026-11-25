#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/sched/resources.hpp"
#include "ok/sched/run_queue.hpp"
#include "ok/sched/task_table.hpp"
#include "ok/sched/unix_rights.hpp"
#include "ok/sched/unix_pathname_sdk_profile.hpp"
#include "ok/sched/descriptor_reservation.hpp"
#include "ok/sched/pty_pair.hpp"
#include "ok/sched/signal_mask.hpp"
#include "ok/sched/deadline.hpp"
#include "ok/sched/console_input.hpp"
#include "ok/sched/console_control.hpp"
#include "ok/sched/terminal.hpp"
#include "ok/sched/terminal_open.hpp"
#include "ok/sched/job_control.hpp"
#include "ok/sched/wait_registration.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_section.hpp"
#include "ok/smp/lock.hpp"
#include "ok/smp/smp.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <concepts>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ok::net { class NativeUdpService; class NativeTcpStreamService; }
namespace ok::user::native { class SharedMemoryFiles; }
namespace ok::sched
{

inline constexpr ProcessId user_init_pid = 1;
// Common Linux-visible task namespace; zero is reserved for boot/service context.
// Linux robust futex owner words reserve the upper two bits for state flags.
inline constexpr u64 max_task_id = 0x3fffffffU;
inline constexpr usize max_process_name = 32;
inline constexpr u8 scheduler_min_priority = 0;
inline constexpr u8 scheduler_default_priority = 16;
inline constexpr u8 scheduler_max_priority = 31;
inline constexpr u16 cpu_affinity_any = 0xffffu;
// Marks a thread that is neither on a CPU nor placed on a run queue.
inline constexpr smp::CpuId no_cpu = 0xffff;

struct PipeReadiness
{
    usize buffered{0};
    usize available{0};
    bool readers{false};
    bool writers{false};
};

struct OrphanReapProgress
{
    // Counts erased records, including an erase followed by a failing hook.
    usize reaped{};
    ProcessId failed_pid{};
    bool failed_after_erase{};
};

enum class ProcessState : u8
{
    created,
    runnable,
    running,
    blocked,
    zombie,
    exited,
};

enum class SchedulingMode : u8
{
    cooperative,
    round_robin,
    per_cpu_round_robin,
};

enum class ProcessExecution : u8
{
    kernel_thread,
    user_process,
};

enum class ProcessCpuAccounting : u8
{
    accounted,
    passive,
};

struct ThreadControlBlock
{
    ThreadId tid{0};
    ProcessId owner{0};
    ProcessState state{ProcessState::created};
    arch::CpuContext context{};
    arch::execution::UserFrame native_frame{};
    bool native_active{false};
    bool syscall_pending{false};
    DeadlineWait native_deadline{};
    WaitRegistration wait_registration{};
    u32 console_wait_epoch{}; // Original input generation survives readiness wakes.
    // The process's continue generation when a console or PTY data wait was
    // armed; a continue since then re-runs the job-control check.
    u32 terminal_gate_mark{};
    ThreadLocalState tls{};
    SignalMask native_signal_mask{};
    memory::KernelEntryStackHandle entry_stack{};
    uptr kernel_stack_top{0};
    ThreadWaitReason wait_reason{ThreadWaitReason::none};
    u64 wait_key{0};
    u8 priority{scheduler_default_priority};
    u64 dispatch_count{0};
    u64 cpu_time_ticks{0};
    smp::CpuId last_cpu{0};
    // The CPU executing this thread, mirrored by that CPU's current slot.
    smp::CpuId on_cpu{no_cpu};
    // The run queue holding this thread's single entry. Dispatch and exit drop
    // the entry; one whose thread blocked meanwhile is discarded when popped.
    smp::CpuId queued_cpu{no_cpu};
    // Whether this fixed slot holds a thread (task_table.hpp). A live slot's
    // state is never exited; an exited record keeps the slot until reused.
    // The per-thread affinity mask is a slot word: ProcessSlot::thread_affinity.
    ThreadSlotState slot_state{ThreadSlotState::free};
};

// A thread whose allowed CPUs all went offline (docs/modules/sysfs.md): its
// mask was reset to every CPU, as Linux does when it breaks affinity.
struct AffinityBroken
{
    ProcessId pid{};
    ThreadId tid{};
    smp::CpuId cpu{}; // The lowest CPU it was confined to.
};

struct CpuSchedulingStats
{
    u64 dispatches{0};
    u64 busy_dispatches{0};
    ProcessId current_pid{0};
    ThreadId current_tid{0};
};

struct ScheduleRequest
{
    std::string_view name{};
    arch::CpuContext initial_context{};
    u8 priority{scheduler_default_priority};
    u16 cpu_affinity_mask{cpu_affinity_any};
    user::Credentials credentials{user::kernel_credentials()};
    bool background{true};
    ProcessCpuAccounting cpu_accounting{ProcessCpuAccounting::accounted};
};

class ProcessControlBlock
{
  public:
    ProcessControlBlock() = default;
    ProcessControlBlock(ProcessId pid, std::string_view name, bool background = false);
    Status configure(ProcessId pid, ProcessId parent, std::string_view name);
    Status add_thread(ThreadId tid, arch::CpuContext context, ProcessState state);
    Status add_child(ProcessId child);
    Status remove_child(ProcessId child);
    [[nodiscard]] ProcessId parent_pid() const
    {
        return parent_pid_;
    }
    [[nodiscard]] ProcessId process_group_id() const { return group_.id; }
    [[nodiscard]] ProcessId session_id() const { return session_.id; }
    [[nodiscard]] bool has_execed() const { return has_execed_; }
    void set_parent(ProcessId parent)
    {
        parent_pid_ = parent;
    }
    void set_exit_code(i32 code)
    {
        exit_code_ = code;
        termination_signal_ = 0;
    }
    [[nodiscard]] i32 exit_code() const
    {
        return exit_code_;
    }
    void set_termination_signal(u8 signal)
    {
        termination_signal_ = signal;
    }
    [[nodiscard]] const JobControlState &job_control() const { return job_; }
    [[nodiscard]] i32 wait_status() const
    {
        return termination_signal_ != 0 ? static_cast<i32>(termination_signal_)
                                        : static_cast<i32>((static_cast<u32>(exit_code_) & 0xffu) << 8);
    }
    [[nodiscard]] auto &children()
    {
        return children_;
    }
    [[nodiscard]] const auto &children() const
    {
        return children_;
    }
    [[nodiscard]] FileDescriptorTable &fd_table()
    {
        return resources_.files;
    }
    [[nodiscard]] const FileDescriptorTable &fd_table() const
    {
        return resources_.files;
    }
    [[nodiscard]] MemoryMap &memory_map()
    {
        return resources_.memory;
    }
    [[nodiscard]] const MemoryMap &memory_map() const
    {
        return resources_.memory;
    }
    [[nodiscard]] ProcessResources &resources()
    {
        return resources_;
    }
    [[nodiscard]] const ProcessResources &resources() const
    {
        return resources_;
    }
    [[nodiscard]] SignalState &signals()
    {
        return signals_;
    }

    [[nodiscard]] ProcessId pid() const
    {
        return pid_;
    }
    [[nodiscard]] u32 console_epoch() const { return console_epoch_; }
    [[nodiscard]] bool ctty_detached() const { return ctty_detached_; }
    [[nodiscard]] std::string_view name() const
    {
        return name_.view();
    }
    [[nodiscard]] ProcessState state() const
    {
        return state_;
    }
    void set_state(ProcessState state)
    {
        state_ = state;
    }
    [[nodiscard]] bool background() const
    {
        return background_;
    }
    void set_background(bool background)
    {
        background_ = background;
    }
    [[nodiscard]] ProcessExecution execution() const
    {
        return execution_;
    }
    void set_execution(ProcessExecution execution)
    {
        execution_ = execution;
    }
    [[nodiscard]] u64 address_space_id() const
    {
        return address_space_id_;
    }
    void set_address_space_id(u64 address_space_id)
    {
        address_space_id_ = address_space_id;
    }
    [[nodiscard]] const user::Credentials &credentials() const
    {
        return credentials_;
    }
    void set_credentials(user::Credentials credentials)
    {
        credentials_ = credentials;
    }
    [[nodiscard]] u8 priority() const
    {
        return priority_;
    }
    void set_priority(u8 priority)
    {
        priority_ = priority;
    }
    [[nodiscard]] u16 cpu_affinity_mask() const
    {
        return cpu_affinity_mask_;
    }
    void set_cpu_affinity_mask(u16 mask)
    {
        cpu_affinity_mask_ = mask;
    }
    [[nodiscard]] bool can_run_on(smp::CpuId cpu) const
    {
        if (cpu >= smp::max_cpus)
        {
            return false;
        }
        return (cpu_affinity_mask_ & static_cast<u16>(1u << cpu)) != 0;
    }
    [[nodiscard]] u64 dispatch_count() const
    {
        return dispatch_count_;
    }
    [[nodiscard]] u64 cpu_time_ticks() const
    {
        return cpu_time_ticks_;
    }
    [[nodiscard]] u64 accounted_cpu_time_ticks() const
    {
        return accounted_cpu_time_ticks_;
    }
    [[nodiscard]] smp::CpuId last_cpu() const
    {
        return last_cpu_;
    }
    [[nodiscard]] ProcessCpuAccounting cpu_accounting() const
    {
        return cpu_accounting_;
    }
    void set_cpu_accounting(ProcessCpuAccounting accounting)
    {
        cpu_accounting_ = accounting;
    }
    void record_dispatch(smp::CpuId cpu, bool account_cpu)
    {
        ++dispatch_count_;
        ++cpu_time_ticks_;
        if (account_cpu)
        {
            ++accounted_cpu_time_ticks_;
        }
        last_cpu_ = cpu;
    }
    // Fixed TCB slots (task_table.hpp): slot 0 is the group leader (tid ==
    // pid) for every process; a TCB never moves while its slot is in use.
    using LiveThreads = ThreadSlotRange<ThreadControlBlock, LiveThreadSlot>;
    using ConstLiveThreads = ThreadSlotRange<const ThreadControlBlock, LiveThreadSlot>;
    using ThreadRecords = ThreadSlotRange<ThreadControlBlock, RecordedThreadSlot>;
    using ConstThreadRecords = ThreadSlotRange<const ThreadControlBlock, RecordedThreadSlot>;
    // Threads that have not exited, in slot order.
    [[nodiscard]] LiveThreads live_threads() { return {threads_.data(), threads_.size()}; }
    [[nodiscard]] ConstLiveThreads live_threads() const { return {threads_.data(), threads_.size()}; }
    // Live threads and exited records, in slot order: what still holds a slot.
    [[nodiscard]] ThreadRecords thread_records() { return {threads_.data(), threads_.size()}; }
    [[nodiscard]] ConstThreadRecords thread_records() const { return {threads_.data(), threads_.size()}; }
    [[nodiscard]] usize live_count() const { return live_threads().size(); }
    [[nodiscard]] usize record_count() const { return thread_records().size(); }
    // The leader's slot, whatever its state; the process-wide records hang
    // off it (signal queue, shutdown owner, the thread exec keeps).
    [[nodiscard]] ThreadControlBlock &leader() { return threads_[0]; }
    [[nodiscard]] const ThreadControlBlock &leader() const { return threads_[0]; }
    // The leader while it lives, else the lowest live slot; null without one.
    [[nodiscard]] ThreadControlBlock *main_thread();
    [[nodiscard]] const ThreadControlBlock *main_thread() const;
    // A slot by position, whatever its state (tests and slot walks).
    [[nodiscard]] ThreadControlBlock &thread_slot(usize slot) { return threads_[slot]; }
    [[nodiscard]] const ThreadControlBlock &thread_slot(usize slot) const { return threads_[slot]; }
    // The position of a TCB of this process; max_threads_per_process otherwise.
    [[nodiscard]] usize slot_of(const ThreadControlBlock &thread) const
    {
        const auto *base = threads_.data();
        for (usize i = 0; i < threads_.size(); ++i)
            if (base + i == &thread) return i;
        return max_threads_per_process;
    }
    // The process slot the scheduler placed this PCB in; 0xffff outside one.
    [[nodiscard]] u16 process_slot() const { return process_slot_; }
    // A thread's ThreadHandle index (process slot * 8 + thread slot).
    [[nodiscard]] u16 thread_index(const ThreadControlBlock &thread) const
    {
        const auto slot = slot_of(thread);
        return process_slot_ == invalid_thread_index || slot == max_threads_per_process
                   ? invalid_thread_index
                   : sched::thread_index(process_slot_, slot);
    }
    // The first free slot, or null when all eight hold a record.
    [[nodiscard]] ThreadControlBlock *free_thread_slot()
    {
        for (auto &thread : threads_)
            if (thread.slot_state == ThreadSlotState::free) return &thread;
        return nullptr;
    }

  private:
    friend class SchedulerState;
    friend struct ProcessTestAccess; // Hosted tests only.
    ProcessId pid_{0};
    u32 console_epoch_{}; // Only the scheduler may issue or inherit membership.
    ProcessId parent_pid_{0};
    ProcessGroup group_{};
    Session session_{};
    bool ctty_detached_{}; // TIOCNOTTY by a non-leader (S8); fork inherits, setsid clears.
    // Linux tty_old_pgrp (S8): a session leader whose terminal was hung up
    // keeps that terminal's foreground group (and the terminal's storage index,
    // whose outbox carries the signals) for its exit. Not inherited by fork.
    ProcessId hangup_old_foreground_{};
    u16 hangup_terminal_{};
    bool has_execed_{}; // Successful image replacement since this process was forked.
    SignalState signals_{};
    ProcessResources resources_{};
    StaticVector<ProcessId, max_process_children> children_{};
    i32 exit_code_{0};
    u8 termination_signal_{0};
    JobControlState job_{}; // Only the scheduler stops or continues a process.
    FixedString<max_process_name> name_{};
    ProcessState state_{ProcessState::created};
    bool background_{false};
    ProcessExecution execution_{ProcessExecution::kernel_thread};
    u64 address_space_id_{0};
    user::Credentials credentials_{user::kernel_credentials()};
    u8 priority_{scheduler_default_priority};
    u16 cpu_affinity_mask_{cpu_affinity_any};
    u64 dispatch_count_{0};
    u64 cpu_time_ticks_{0};
    u64 accounted_cpu_time_ticks_{0};
    smp::CpuId last_cpu_{0};
    ProcessCpuAccounting cpu_accounting_{ProcessCpuAccounting::accounted};
    u16 process_slot_{invalid_thread_index};
    std::array<ThreadControlBlock, max_threads_per_process> threads_{};
};

using ProcessSlot = BasicProcessSlot<ProcessControlBlock>;
using ProcessPin = BasicProcessPin<ProcessSlot>;
using ProcessRange = BasicProcessRange<ProcessSlot, ProcessControlBlock>;
using ConstProcessRange = BasicProcessRange<const ProcessSlot, const ProcessControlBlock>;
using CpuSlot = BasicCpuSlot<ProcessControlBlock, ThreadControlBlock>;

// Policies are kernel build-time choices. The scheduler stores its policy by
// value so copies cannot retain a pointer into a different scheduler instance.
template <typename T>
concept SchedulerPolicy =
    requires(T &policy, ConstProcessRange processes, ProcessId current, smp::CpuId cpu) {
        { policy.pick_next(processes, current, cpu) } -> std::same_as<Result<ProcessId>>;
    };

class RoundRobinPolicy final
{
  public:
    [[nodiscard]] constexpr std::string_view name() const
    {
        return "round-robin";
    }
    Result<ProcessId> pick_next(ConstProcessRange processes, ProcessId current, smp::CpuId cpu);
};

// A queue can hold every thread a process table admits. Lock types are a
// build-time choice made here, not a runtime queue interface.
using SchedulerRunQueue = CpuRunQueue<max_thread_slots, smp::SpinLock, ThreadHandle>;
static_assert(RunQueue<SchedulerRunQueue>);

// One open description's offset and flags across one offset-dependent I/O,
// with the provider call that depends on them (ok/smp/lock.hpp rank
// ofd_position). Taken in both modes: under the kernel lock and in a kernel
// section (ok/smp/kernel_section.hpp). No path holds two of them.
using DescriptionPositionLock = smp::RankedSpinLock<smp::LockRank::ofd_position>;

// Holds a description's position lock for a scope; a null lock holds nothing.
class DescriptionPositionGuard final
{
  public:
    explicit DescriptionPositionGuard(DescriptionPositionLock *lock) noexcept : lock_(lock)
    {
        if (!lock_) return;
        if (!smp::in_section())
        {
            lock_->lock();
            return;
        }
        // A kernel section waits serving this CPU's interrupts: the holder
        // may hold the kernel lock and wait for a device line routed here.
        if (lock_->try_lock()) return;
        if (lock_->owned_by_current_cpu())
        {
            // Re-entry on this CPU would spin forever, and every drainer of
            // the process with it: report it and go on under the outer hold.
            if constexpr (smp::lock_debug_enabled)
                smp::lock_rank_violation({smp::LockViolationKind::self_deadlock, DescriptionPositionLock::rank,
                                          DescriptionPositionLock::rank, lock_, smp::current_cpu()});
            smp::detail::section_assert(&smp::SectionCpu::section_asserts, "section re-entered a position lock");
            lock_ = nullptr;
            return;
        }
        const smp::SpinningScope spinning;
        while (!lock_->try_lock())
        {
            smp::serve_while_spinning();
            arch::ArchOperations::spin_pause();
        }
    }
    ~DescriptionPositionGuard() { release(); }
    DescriptionPositionGuard(const DescriptionPositionGuard &) = delete;
    DescriptionPositionGuard &operator=(const DescriptionPositionGuard &) = delete;
    // Gives the lock up before the scope ends.
    void release() noexcept
    {
        if (lock_) lock_->unlock();
        lock_ = nullptr;
    }

  private:
    DescriptionPositionLock *lock_;
};

// Nonvirtual shared storage/mechanisms keep each policy instantiation small.
// Construction and dispatch are protected for use by BasicScheduler.
class SchedulerState
{
  protected:
    SchedulerState();
    ~SchedulerState() = default;
    Result<ProcessId> dispatch_selected(smp::CpuId cpu, Result<ProcessId> next, ThreadId requested = 0);

  public:
    Status close_shutdown_admission();
    [[nodiscard]] bool shutdown_admission_closed() const { return shutdown_admission_closed_; }
    void set_mode(SchedulingMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] SchedulingMode mode() const
    {
        return mode_;
    }
    Result<ProcessId> create_process(std::string_view name, arch::CpuContext initial_context);
    Result<ProcessId> create_user_process(std::string_view name, arch::CpuContext initial_context);
    // PID 1 is reserved independently of kernel task creation order.
    Result<ProcessId> create_init_process(arch::CpuContext initial_context);
    Result<ProcessId> create_background_process(std::string_view name, arch::CpuContext initial_context);
    Result<ThreadId> create_thread(ProcessId pid, arch::CpuContext initial_context);
    // Prepare only the unpublished TCB; do not mutate scheduler storage or its
    // identity. On failure the callback must retire every resource it acquired.
    // Capacity is preflighted, so successful preparation has a nonthrowing commit.
    template <typename Prepare>
        requires requires(Prepare &&prepare, ThreadControlBlock &thread) {
            { std::forward<Prepare>(prepare)(thread) } -> std::same_as<Status>;
        }
    Result<ThreadId> create_thread_prepared(ProcessId pid, arch::CpuContext initial_context, Prepare &&prepare)
    {
        smp::check_exclusive("SchedulerState::create_thread_prepared");
        smp::check_drained(pid, "SchedulerState::create_thread_prepared");
        if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
        auto *process = find(pid);
        if (process == nullptr)
        {
            return Status::not_found("process not found");
        }
        if (process->state() == ProcessState::exited || process->state() == ProcessState::zombie)
        {
            return Status::invalid_argument("cannot create a thread for an exited process");
        }
        if (process->live_count() == 0) return Status::invalid_argument("cannot create a thread in a retired group");
        // An exited record that holds nothing is reused before a free slot;
        // the leader's slot never is.
        ThreadControlBlock *retired = nullptr;
        for (auto &thread : process->thread_records())
            if (thread.slot_state == ThreadSlotState::exited && thread.tid != pid && !thread.native_active &&
                !thread.wait_registration.active() && !thread.entry_stack.pool && !thread.kernel_stack_top)
            {
                retired = &thread;
                break;
            }
        ThreadControlBlock *slot = retired ? retired : process->free_thread_slot();
        if (!slot)
            return Status::would_block("thread table capacity exceeded");
        if (next_task_id_ > max_task_id)
            return Status::overflow("task identity exhausted");
        const auto tid = next_task_id_++;
        ThreadControlBlock created{
            .tid = tid,
            .owner = pid,
            .state = ProcessState::created,
            .context = initial_context,
            .priority = process->priority(),
        };
        if (auto status = std::forward<Prepare>(prepare)(created); !status)
            return status;
        created.state = ProcessState::runnable;
        created.slot_state = ThreadSlotState::live;
        if (retired) forget_queued(*process, *retired);
        *slot = created;
        // A new thread starts with every CPU; clone then copies its caller's mask.
        store_thread_affinity(*process, *slot, cpu_affinity_any);
        if (process->state() == ProcessState::created || process->state() == ProcessState::blocked)
        {
            process->set_state(ProcessState::runnable);
        }
        return tid;
    }

    // Serialized retirement only; caller drains external waits and releases
    // thread-owned resources first. Process/VM/FD ownership remains unchanged.
    Result<usize> exit_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu = smp::current_cpu());
    Result<ProcessId> spawn(ScheduleRequest request);
    Status set_credentials(ProcessId pid, user::Credentials credentials);
    Status set_priority(ProcessId pid, u8 priority);
    Status set_cpu_affinity(ProcessId pid, u16 cpu_affinity_mask);
    Status set_cpu_accounting(ProcessId pid, ProcessCpuAccounting accounting);
    Status kill_process(ProcessId pid);
    Status activate(ProcessId pid, smp::CpuId cpu = smp::current_cpu());
    Status activate_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu = smp::current_cpu());
    Result<ProcessId> fork_process(ProcessId parent, bool publish = true);
    // Trusted serialized process mechanisms. Native and Linux syscall boundaries
    // authenticate the caller before entering these operations.
    [[nodiscard]] Result<ProcessId> process_group(ProcessId caller, i32 pid) const;
    [[nodiscard]] Result<ProcessId> session_id(ProcessId caller, i32 pid) const;
    Status set_process_group(ProcessId caller, i32 pid, i32 pgid);
    [[nodiscard]] Result<ProcessId> create_session(ProcessId caller);
    // Every check replace_image makes before it changes anything, except the
    // entry stack escrow: exec runs it before evicting sibling threads.
    [[nodiscard]] Status preflight_replace_image(ProcessId pid, const arch::CpuContext &context) const;
    Status replace_image(ProcessId pid, arch::CpuContext context, const MemoryMap &memory);
    Status exit_process(ProcessId pid, i32 code);
    Status terminate_process(ProcessId pid, u8 signal);
    Result<i32> wait_process(ProcessId parent, ProcessId child);
    Status reparent_children(ProcessId from, ProcessId to);
    [[nodiscard]] ProcessId user_adoption_target(ProcessId departing) const;
    Status reap_process(ProcessId pid);
    // Serialized completion only: unlink without moving or erasing PCB/TCB storage.
    [[nodiscard]] Status detach_completed_child_to_reaper(ProcessId parent, ProcessId child);
    // Stops at the first failure; progress is reset and filled on every call.
    [[nodiscard]] Status reap_orphans_checked(OrphanReapProgress &progress);
    // Call only at service boundaries: erasing records invalidates PCB/TCB pointers.
    usize reap_orphans();
    [[nodiscard]] ProcessResources &boot_resources()
    {
        return boot_resources_;
    }
    [[nodiscard]] const ProcessResources &boot_resources() const
    {
        return boot_resources_;
    }
    [[nodiscard]] OpenFileDescription *open_description(u16 id);
    [[nodiscard]] const OpenFileDescription *open_description(u16 id) const;
    // The position lock of a description in this table; null for any other.
    [[nodiscard]] DescriptionPositionLock *position_lock(const OpenFileDescription *description);
    // The process whose resources these are; 0 for the boot resources.
    [[nodiscard]] ProcessId resources_owner(const ProcessResources &resources) const;
    // Rebinding is allowed only after every UDP description, including an
    // unpublished zero-reference reservation, has been retired.
    Status set_native_udp_service(net::NativeUdpService &service);
    Status set_native_tcp_service(net::NativeTcpStreamService &service);
    Status set_shared_memory_files(user::native::SharedMemoryFiles &service);
    [[nodiscard]] user::native::SharedMemoryFiles *shared_memory_files() const { return shared_memory_files_; }
    [[nodiscard]] net::NativeTcpStreamService *native_tcp_service() const { return native_tcp_service_; }
    Result<u16> allocate_description(OpenFileDescription description);
    Result<DescriptorReservation> prepare_descriptor(u64 pid);
    Result<PreparedPtyPair> prepare_pty_pair(u64 pid, u32 flags = 0);
    using PreparedPtyRead = private_pty::RawPtyTable<>::PreparedRead;
    // `timer_expired`: a retry after the reader's VTIME timer ran out.
    Result<PreparedPtyRead> prepare_pty_read(u16 description, usize count, bool timer_expired = false,
                                             private_pty::ReadTimer timer = {});
    using PtyReadState = private_pty::RawPtyTable<>::ReadState;
    Result<PtyReadState> pty_read_state(u16 description, usize count, private_pty::ReadTimer timer = {}) const;
    Result<bool> pty_write_progress(u16 description, std::byte next) const;
    Result<usize> commit_pty_read(PreparedPtyRead &preview);
    // Keys may spuriously collide across pair generations. Retained handle and
    // wait-ticket validation must compare the complete generation separately.
    static constexpr u64 pty_wait_key(private_pty::Handle endpoint)
    { return (static_cast<u64>(endpoint.slot) << 1) | (endpoint.side == private_pty::Side::slave ? 1u : 0u); }
    Result<usize> read_pty_description(u16 description, std::span<std::byte> output);
    Result<usize> write_pty_description(u16 description, std::span<const std::byte> input);
    Result<private_pty::Readiness> pty_description_readiness(u16 description) const;
    [[nodiscard]] private_pty::Audit pty_audit() const { return pty_transport_.audit(); }
    // ptmx-style pairs (docs/modules/tty.md section 8). The devfs open path
    // (S6) binds a fresh provider-free description that holds exactly one
    // reference; each bound description owns one endpoint open, shared by dup,
    // fork and SCM_RIGHTS, and its final release retires that open.
    // bind_pty_multiplexer creates the pair (slave locked and dormant) and
    // returns the master endpoint; bind_pty_slave admits one more slave open.
    Result<private_pty::Handle> bind_pty_multiplexer(u16 description);
    Status bind_pty_slave(u16 description, private_pty::Handle slave);
    [[nodiscard]] Result<u32> pty_index(u16 description) const;
    Status set_pty_lock(u16 description, bool locked);
    // Each endpoint's open count equals the descriptions bound to it, outside
    // an unpublished prepared raw pair (whose opens precede publication).
    [[nodiscard]] Status terminal_audit() const;
    // Terminal device nodes (docs/modules/tty.md sections 2 and 3). open()
    // binds a fresh provider-free description holding exactly one reference;
    // every check precedes the first change, and the node is re-validated
    // here (index, generation, lock and liveness), never trusted. /dev/tty is
    // the caller's derived controlling terminal or no_device (ENXIO);
    // /dev/console never acquires; /dev/ptmx gives no_space (ENOSPC) when every
    // pair slot, raw pairs included, is in use; a /dev/pts/N open without
    // O_NOCTTY by a session leader without a terminal acquires an unowned,
    // readable slave (Linux tty_open_proc_set_tty).
    Result<TerminalOpenResult> open_terminal(ProcessId pid, u16 description, const TerminalOpenRequest &request);
    // The node a terminal description reports to fstat; not_terminal for raw
    // pairs and every other description.
    [[nodiscard]] Result<TerminalStatus> terminal_status(u16 description) const;
    // chmod and chown of a /dev/pts entry (owner or root; chown root only).
    Status change_pts_mode(fs::TerminalNode node, u32 mode, user::Credentials caller);
    Status change_pts_owner(fs::TerminalNode node, u32 uid, u32 gid, user::Credentials caller);
    // The entry a description bound to a ptmx slave names; not_terminal otherwise.
    [[nodiscard]] Result<fs::TerminalNode> pts_node(u16 description) const;
    [[nodiscard]] fs::TerminalNamespace &terminal_namespace() { return terminal_namespace_; }
    [[nodiscard]] const fs::TerminalNamespace &terminal_namespace() const { return terminal_namespace_; }
    // Each successful retain owns one reference, released exactly once. A pin
    // keeps the ID, backing node and pipe endpoint alive across fd close/reuse.
    Status retain_description(u16 id);
    Status release_description(u16 id);
    Status install_descriptor(ProcessResources &resources, i32 fd, u16 description, bool close_on_exec = false);
    Status close_descriptor(ProcessResources &resources, i32 fd);
    // Serialized shutdown/exit cleanup. Consume every fd/cwd edge even when a
    // release fails; no memory lock spans provider or endpoint callbacks.
    [[nodiscard]] Status drain_resources(ProcessResources &resources);
    // Caller stops new workload/resource admission first. External description
    // pins/reservations remain owned and return busy; prior cleanup failure is sticky.
    [[nodiscard]] Status drain_all_resources();
    [[nodiscard]] ResourceDrainReport resource_drain_report() const
    {
        return resource_drain_report_;
    }
    [[nodiscard]] usize open_description_count() const;
    Result<std::array<i32, 2>> create_pipe(ProcessResources &resources, bool close_on_exec = false, u32 status_flags = 0);
    Result<std::array<i32, 2>> create_unix_socketpair(ProcessResources &resources, bool close_on_exec = false,
                                                       u32 status_flags = 0);
    // Serialized immediate pathname stream operations; credentials are snapshots
    // supplied by the checked POSIX service, never inferred from a later accept.
    Result<i32> create_unix_socket(ProcessResources &resources, bool close_on_exec = false, u32 status_flags = 0);
    [[nodiscard]] bool is_unix_socket(u16 description) const;
    [[nodiscard]] Status validate_unix_bind(u16 description) const;
    Status bind_unix_socket(u16 description, fs::SocketPathReference &&binding);
    Status listen_unix_socket(u16 description, usize backlog, UnixPeerCredentials credentials);
    Status connect_unix_socket(u16 description, const fs::SocketPathReference &target, UnixPeerCredentials credentials);
    Result<UnixAccept> prepare_unix_accept(ProcessResources &resources, u16 listener,
        bool close_on_exec = false, u32 status_flags = 0);
    [[nodiscard]] Result<UnixPeerCredentials> unix_peer_credentials(u16 description) const;
    [[nodiscard]] Result<UnixSocketReadiness> unix_socket_readiness(u16 description) const;
    [[nodiscard]] usize unix_endpoint_count() const;
    // Read-only COMMIT/final-drain proof; stdio/cwd remain valid until retirement.
    [[nodiscard]] Status shutdown_endpoint_status() const;
    Result<usize> send_unix_rights(ProcessResources &resources, i32 fd, std::span<const std::byte> payload,
                                  std::span<const i32> descriptors, bool nonblocking = false);
    Result<UnixReceive> prepare_unix_receive(ProcessResources &resources, i32 fd, std::span<std::byte> preview,
        usize descriptor_capacity, bool peek = false, bool close_on_exec = false, bool nonblocking = false);
    // Diagnostic collection has the same serialized ownership boundary as fd close.
    Status collect_unix_rights();
    Result<usize> read_pipe(u16 pipe, std::span<std::byte> out, bool nonblocking = false);
    Result<usize> write_pipe(u16 pipe, std::span<const std::byte> in, bool nonblocking = false,
                             bool atomic_request = true);
    // A continuation preserves atomicity of the original request, not its suffix.
    Result<usize> write_pipe_fragment(u16 pipe, std::span<const std::byte> in, bool atomic_request);
    [[nodiscard]] Result<PipeReadiness> pipe_readiness(u16 pipe) const;
    Status block_current(ThreadWaitReason reason, u64 key);
    // Serialized boot-CPU wait ownership. Tokens never identify a TCB address.
    Result<WaitTicket> register_current_wait(ThreadWaitReason reason, u64 key);
    // `source` names what made the wait ready; completed waits are counted by
    // the source their registration recorded.
    bool complete_wait(WaitTicket ticket, WaitCompletion completion, WakeSource source = WakeSource::thread);
    [[nodiscard]] ThreadControlBlock *find_thread(ProcessId pid, ThreadId tid);
    [[nodiscard]] ThreadControlBlock *current_thread();
    void wake_waiters(ThreadWaitReason reason, u64 key, WakeSource source = WakeSource::thread);
    // Ready and timed-out waits completed so far, by wake source.
    [[nodiscard]] u64 wakes(WakeSource source) const { return wakes_[static_cast<usize>(source)]; }
    // Native single-console input; descriptor possession never grants membership.
    Result<u32> claim_console(ProcessId target);
    Status revoke_console(u32 expected_epoch);
    Status flush_console(u32 expected_epoch);
    [[nodiscard]] bool console_authorized(ProcessId pid, u32 expected_epoch = 0) const;
    [[nodiscard]] Status console_read_authority(ProcessId pid, u32 expected_epoch = 0) const;
    [[nodiscard]] bool console_readable(ProcessId pid, u32 expected_epoch) const;
    // Lease authority and a blocking read of `count` bytes that would complete now.
    [[nodiscard]] bool console_read_ready(ProcessId pid, u32 expected_epoch, usize count) const;
    [[nodiscard]] Status shutdown_console_status() const;
    // Console reads are planned, copied by the caller, then committed, so a
    // failed copy-out consumes nothing. A plan that must wait gives would_block.
    Result<TerminalLdisc::ReadPlan> prepare_console_read(ProcessId pid, u32 expected_epoch, usize count,
                                                         bool nonblocking);
    usize preview_console_read(const TerminalLdisc::ReadPlan &plan, std::span<std::byte> output,
                               usize offset) const;
    Status commit_console_read(const TerminalLdisc::ReadPlan &plan);
    // One ingress byte through the console line discipline. ISIG signals go to
    // the console outbox; echo stays in the ldisc for the runtime to emit.
    // `source` is the console's: interrupt once its UART line is bound.
    TerminalLdisc::Input console_input(char value, WakeSource source = WakeSource::thread);
    // Terminals (docs/modules/tty.md sections 1, 4 and 5). The console is
    // terminal 0; the runtime moves echo and output and drains the outbox.
    [[nodiscard]] Terminal *terminal(TerminalRef ref);
    [[nodiscard]] const Terminal *terminal(TerminalRef ref) const;
    // Every terminal's storage by index (0 console, 1 + slot), live or not,
    // for the outbox drain.
    static constexpr usize terminal_count = 1 + private_pty::RawPtyTable<>::pair_capacity;
    [[nodiscard]] Terminal &terminal_storage(usize index);
    [[nodiscard]] Terminal &console_terminal() { return console_terminal_; }
    [[nodiscard]] const Terminal &console_terminal() const { return console_terminal_; }
    [[nodiscard]] Result<TerminalSubject> terminal_subject(u16 description) const;
    [[nodiscard]] std::optional<TerminalRef> controlling_terminal(const ProcessControlBlock &process) const;
    [[nodiscard]] TerminalJobFacts terminal_job_facts(ProcessId pid, TerminalRef ref) const;
    Status acquire_terminal(TerminalRef ref, ProcessControlBlock &caller, bool readable, AcquireMode mode);
    // Association checks of the running caller; they precede every copy-in.
    Status terminal_check(const TerminalSubject &subject, ConsoleControlOperation operation);
    Result<TerminalReply> terminal_control(const TerminalSubject &subject, const TerminalRequest &request);
    // Job-control wait key of a terminal: 0 for the console, 1 + slot for a PTY.
    [[nodiscard]] static constexpr u64 job_control_key(TerminalRef ref) { return ref.index; }
    // Trusted runtime ingress failure closes ownership before returning to userland.
    void abort_console_input();
    // Clears every CPU slot and moves queue entries off CPUs beyond the count;
    // the online set keeps only its configured CPUs.
    Status configure_cpus(usize cpu_count);
    // CPU 0 stays online; every online CPU must be configured. Entries already
    // queued on a CPU that goes offline stay there and are not stolen.
    Status set_online_cpus(u32 mask);
    // Hotplug: a configured secondary CPU leaves the online set and every
    // entry queued on it moves to an online queue (stale ones are dropped),
    // so no thread is lost or queued twice and nothing waits on it. Its
    // current slot stays until that CPU vacates it itself.
    Status offline_cpu(smp::CpuId cpu);
    // A configured CPU joins the online set again; it starts with its empty queue.
    Status online_cpu(smp::CpuId cpu);
    // A thread's own mask (sched_setaffinity), kept in its process slot's
    // thread_affinity word; every CPU for a TCB outside the slot table.
    [[nodiscard]] u16 thread_affinity(const ProcessControlBlock &process, const ThreadControlBlock &thread) const;
    // Stores a thread's own mask without any check (clone, fork, exec, reset).
    void store_thread_affinity(const ProcessControlBlock &process, const ThreadControlBlock &thread, u16 mask);
    // The CPUs a thread may run on: its process mask and its own.
    [[nodiscard]] u16 effective_affinity(const ProcessControlBlock &process, const ThreadControlBlock &thread) const
    {
        return static_cast<u16>(process.cpu_affinity_mask() & thread_affinity(process, thread));
    }
    // Whether the thread may run on `cpu`; CPU 0 also admits a thread whose
    // effective mask names no configured CPU (the select_cpu fallback).
    [[nodiscard]] bool thread_admits(const ProcessControlBlock &process, const ThreadControlBlock &thread,
                                     smp::CpuId cpu) const;
    // A thread none of whose allowed CPUs is online gets every CPU back (its
    // thread mask resets) and is recorded once as broken affinity. Returns
    // whether it was reset. Every placement of a thread calls it first.
    bool repair_affinity(ProcessControlBlock &process, ThreadControlBlock &thread);
    // sched_setaffinity on a thread: invalid_argument when the process mask
    // and `mask` share no online CPU, else the thread mask becomes `mask`.
    Status set_thread_affinity(ProcessControlBlock &process, ThreadControlBlock &thread, u16 mask);
    // A live thread of a live process by its id alone (sched_*affinity name
    // threads by tid): a scan of the slot table.
    [[nodiscard]] ThreadControlBlock *find_thread_by_tid(ThreadId tid);
    // The live thread a handle names; null once its slot holds another tid.
    [[nodiscard]] ThreadControlBlock *thread_at(ThreadHandle handle);
    [[nodiscard]] const ThreadControlBlock *thread_at(ThreadHandle handle) const;
    // The PCB of the slot a thread index falls in, whatever its lifecycle.
    [[nodiscard]] ProcessControlBlock &slot_process(u16 index) { return slots_[index / max_threads_per_process].pcb; }
    // A thread's handle; invalid for a TCB outside the slot table.
    [[nodiscard]] static ThreadHandle handle_of(const ProcessControlBlock &process, const ThreadControlBlock &thread)
    {
        return {process.thread_index(thread), static_cast<u32>(thread.tid)};
    }
    // Pins the [live, zombie] process `pid` (spec 3.2); empty when none.
    [[nodiscard]] ProcessPin pin(ProcessId pid);
    // The process slot holding `pid` in any lifecycle but free; side tables
    // indexed by process slot check their stored pid against it.
    [[nodiscard]] std::optional<usize> slot_of(ProcessId pid) const;
    // Whether a thread is queued on `cpu` (run queues hold handles).
    [[nodiscard]] bool queued_on(smp::CpuId cpu, ThreadRef ref) const;
    // Broken-affinity records not yet reported, oldest first; the ring keeps
    // the last 16, the count every one.
    [[nodiscard]] std::optional<AffinityBroken> take_affinity_broken();
    [[nodiscard]] u64 affinity_broken() const { return affinity_broken_total_; }
    // Once native userland started, the kernel tick and the preemption tick
    // place only kernel_thread records: native threads belong to the
    // runtime's step, which honors their thread masks.
    void set_native_userland(bool started) { native_userland_ = started; }
    [[nodiscard]] bool native_userland() const { return native_userland_; }
    // Round robin over the kernel_thread records allowed on `cpu`, after the
    // one it runs; dispatches the choice there.
    Result<ProcessId> schedule_kernel_thread_on_cpu(smp::CpuId cpu);
    [[nodiscard]] u32 online_cpus() const { return online_cpus_; }
    // Per-CPU run queues are a mechanism; nothing enqueues implicitly, so a
    // thread woken while still on its CPU must be enqueued again by whoever
    // vacates that CPU (the native runtime enqueues every runnable thread that
    // is off-CPU on each step). The
    // caller serializes scheduler state, while each queue lock guards only its
    // ring, so no path ever holds two queue locks.
    // Succeeds without change when the thread is on a CPU or already queued.
    Status enqueue_thread(ProcessId pid, ThreadId tid);
    // Placement: the last CPU of a thread that has run, while it is online
    // and allowed; otherwise the allowed online CPU with the shortest queue, lowest id on a
    // tie; with no allowed CPU online, CPU 0, which is never offline. CPU 0
    // runs the entry only if the affinity names no configured CPU; otherwise
    // the entry waits there until a CPU it allows comes online and takes it.
    [[nodiscard]] smp::CpuId select_cpu(ProcessControlBlock &process, ThreadControlBlock &thread);
    // Pops this online CPU's queue, discarding stale entries and moving
    // entries the affinity no longer allows. An empty queue steals the oldest
    // eligible entry from the longest other online queue it can try-lock,
    // trying shorter victims next. The caller marks the result with mark_on_cpu.
    std::optional<ThreadRef> dequeue_thread(smp::CpuId cpu);
    // Dispatches exactly this thread on an online, vacant CPU, like
    // activate_thread, and refuses a thread running on another CPU.
    Status mark_on_cpu(ProcessId pid, ThreadId tid, smp::CpuId cpu);
    // Vacates the CPU; the thread returns to runnable unless it blocked or exited.
    Status clear_on_cpu(ProcessId pid, ThreadId tid, smp::CpuId cpu);
    [[nodiscard]] const SchedulerRunQueue &run_queue(smp::CpuId cpu) const { return run_queues_.get(cpu); }
    Status set_runnable(ProcessId pid);
    Status block_process(ProcessId pid);
    // Job-control overlay (job_control.inc). A stopped process keeps its
    // threads and waits; the runtime never enters it until it is continued.
    // Meanwhile enqueue_thread, stealing, mark_on_cpu and activate_thread
    // refuse it, and dequeue_thread discards an entry queued before the stop.
    // `signal` is in the process's own numbering. Stopping an already stopped
    // process succeeds without change; a zombie gives already_exists.
    Status stop_process(ProcessId pid, u8 signal);
    // Returns true when the process was stopped.
    bool continue_process(ProcessId pid);
    void consume_job_report(ProcessId pid, bool stop);
    [[nodiscard]] u16 stopped_processes() const { return stopped_count_; }
    [[nodiscard]] bool process_group_orphaned(ProcessId group, ProcessId ignored = 0) const;
    [[nodiscard]] bool process_group_has_stopped(ProcessId group) const;
    // Child-wait key of a process-group selector; pid keys stay below 2^32.
    [[nodiscard]] static constexpr u64 child_group_wait_key(ProcessId group) { return (u64{1} << 32) | group; }

    // No-argument native services act for the thread of the calling CPU.
    [[nodiscard]] ProcessId current_pid() const
    {
        return current_pid(smp::current_cpu());
    }
    [[nodiscard]] ProcessId current_pid(smp::CpuId cpu) const;
    [[nodiscard]] ThreadId current_tid(smp::CpuId cpu) const;
    [[nodiscard]] usize cpu_count() const
    {
        return cpu_count_;
    }
    // Processes in [live, zombie].
    [[nodiscard]] usize process_count() const { return processes().size(); }
    [[nodiscard]] usize background_process_count() const;
    // The [live, zombie] process `pid`: a scan of the 64 slot pid words.
    [[nodiscard]] ProcessControlBlock *find(ProcessId pid);
    [[nodiscard]] const ProcessControlBlock *find(ProcessId pid) const;
    // The [live, zombie] processes in slot order. The PCBs never move; the
    // caller holds the kernel lock (the task table once it exists).
    [[nodiscard]] ProcessRange processes()
    {
        smp::check_exclusive("SchedulerState::processes");
        return {slots_.data(), slots_.size()};
    }
    [[nodiscard]] ConstProcessRange processes() const
    {
        smp::check_exclusive("SchedulerState::processes");
        return {slots_.data(), slots_.size()};
    }
    // What `cpu` runs: its caller binding, written by that CPU.
    [[nodiscard]] const CpuSlot &cpu_slot(smp::CpuId cpu) const { return cpu_slots_[cpu]; }
    [[nodiscard]] CpuSchedulingStats cpu_stats(smp::CpuId cpu) const;
    [[nodiscard]] u64 total_dispatches() const;
    [[nodiscard]] u64 total_accounted_dispatches() const;
    [[nodiscard]] u8 cpu_usage_percent(smp::CpuId cpu) const;
    [[nodiscard]] u8 process_usage_percent(ProcessId pid) const;

  private:
    [[nodiscard]] static bool thread_running_on_other_cpu(const ThreadControlBlock &thread, smp::CpuId cpu)
    {
        return thread.on_cpu != no_cpu && thread.on_cpu != cpu;
    }
    [[nodiscard]] bool cpu_online(smp::CpuId cpu) const
    {
        return cpu < cpu_count_ && (online_cpus_ & (1u << cpu)) != 0;
    }
    // Drops the thread's queue entry, if any.
    void forget_queued(const ProcessControlBlock &process, ThreadControlBlock &thread);
    // processes() without the kernel-lock check, for the scheduler's own walks.
    [[nodiscard]] ProcessRange process_range() { return {slots_.data(), slots_.size()}; }
    [[nodiscard]] ConstProcessRange process_range() const { return {slots_.data(), slots_.size()}; }
    // The [live, zombie] slot holding `pid`; null otherwise.
    [[nodiscard]] ProcessSlot *visible_slot(ProcessId pid);
    [[nodiscard]] const ProcessSlot *visible_slot(ProcessId pid) const;
    // A free slot for a new process: the next free one after the last
    // allocation, so slot order follows creation order until it wraps.
    [[nodiscard]] ProcessSlot *claim_process_slot();
    // Reaped: the slot is freed now, or by the last unpin.
    void release_process_slot(ProcessSlot &slot);
    // Clears an exited sibling's slot (exec): it holds no resource any more.
    void free_thread_slot(ProcessControlBlock &process, ThreadControlBlock &thread);
    void vacate_cpu_slot(usize cpu);
    void occupy_cpu(smp::CpuId cpu, ProcessControlBlock &process, ThreadControlBlock &thread);
    // The CPU 0 fallback of select_cpu admits a thread whose affinity names no configured CPU.
    [[nodiscard]] bool affinity_admits(const ProcessControlBlock &process, smp::CpuId cpu) const;
    [[nodiscard]] bool affinity_admits(const ProcessControlBlock &process, const ThreadControlBlock &thread,
                                       smp::CpuId cpu) const
    {
        return thread_admits(process, thread, cpu);
    }
    [[nodiscard]] bool eligible_on(ThreadHandle handle, smp::CpuId queue, smp::CpuId cpu) const;
    Result<ProcessId> create_process_with_pid(ProcessId pid, std::string_view name,
                                               arch::CpuContext initial_context);
    [[nodiscard]] Result<ThreadId> pick_next_thread(ProcessControlBlock &process, ThreadId current, smp::CpuId cpu);
    [[nodiscard]] u16 configured_cpu_mask() const;

    void initialize_resources(ProcessResources &resources);
    void release_resources(ProcessResources &resources);
    Status consume_resources(ProcessResources &resources);
    void record_resource_failure(Status status, i32 fd, u16 description, usize mount_id, bool cwd);
    void clear_current(ProcessId pid);
    Status reparent_children_on_exit(ProcessId pid);
    Status finish_process(ProcessId pid, i32 code, u8 signal);
    void wake_job_waiters(const ProcessControlBlock &process);
    [[nodiscard]] ProcessControlBlock *console_caller();
    void retire_console_process(ProcessId pid);
    void cancel_console_epoch(u32 epoch);
    void reset_console_terminal();
    void hangup_console(u32 epoch);
    void hangup_exiting_leader(ProcessControlBlock &process);
    void remember_hangup(ProcessId leader, ProcessId foreground, u16 terminal);
    void clear_association(TerminalRef ref, Terminal &target);
    [[nodiscard]] tty::Termios console_read_termios() const;
    void wake_pty(private_pty::Handle endpoint, u8 wakes);
    Status pty_terminal_check(const TerminalSubject &subject, ConsoleControlOperation operation);
    void terminal_changed(TerminalRef ref);
    Result<private_pty::Handle> bind_pty_multiplexer(u16 description, u32 uid, u32 gid);
    private_pty::RawPtyTable<> pty_transport_{}; // One stable table; handles never cross scheduler domains.
    ConsoleInputLease console_input_{};
    std::array<u64, wake_source_count> wakes_{};
    u16 stopped_count_{};
    Terminal console_terminal_{};
    fs::TerminalNamespace terminal_namespace_{};
    [[nodiscard]] bool pipe_has_endpoint(u16 pipe, bool writable) const;
    friend class UnixReceive;
    friend class UnixAccept;
    friend class DescriptorReservation;
    friend class PreparedPtyPair;
    Result<u64> next_descriptor_token();
    Status validate_descriptor_reservation(const DescriptorReservation &operation) const;
    Status cancel_descriptor_reservation(DescriptorReservation &operation);
    i32 publish_descriptor_reservation(DescriptorReservation &operation, OpenFileDescription description, bool close_on_exec);
    [[nodiscard]] bool canonical_unix_description(u16 id) const;
    [[nodiscard]] usize unix_read_count(u16 pipe, usize requested, u16 &record) const;
    Status release_unix_record(UnixRightsRecord record);
    Status retire_pipe(u16 pipe);
    void consume_unix_bytes(u16 pipe, usize count, u16 record);
    Result<usize> commit_unix_receive(UnixReceive &receive);
    void cancel_unix_receive(UnixReceive &receive);
    enum class UnixEndpointState : u8 { unbound, bound, listening, connected };
    struct UnixEndpoint
    {
        bool used{};
        u64 generation{};
        u16 description{invalid_description};
        UnixEndpointState state{UnixEndpointState::unbound};
        std::optional<fs::SocketPathReference> binding{};
        UnixPeerCredentials listen_credentials{}, peer_credentials{};
        usize backlog{}, pending_count{};
        std::array<u16, max_unix_backlog> pending{};
        u64 accept_token{};
    };
    [[nodiscard]] UnixEndpoint *unix_endpoint(u16 description);
    [[nodiscard]] const UnixEndpoint *unix_endpoint(u16 description) const;
    Result<UnixEndpointHandle> reserve_unix_endpoint();
    Status release_unix_endpoint(UnixEndpointHandle handle);
    Status release_unix_pending(u16 child);
    Result<i32> commit_unix_accept(UnixAccept &operation);
    void cancel_unix_accept(UnixAccept &operation);
    std::array<UnixEndpoint, max_unix_endpoints> unix_endpoints_{};
    u64 next_unix_endpoint_generation_{1};
    std::array<PipeBuffer, max_process_pipes> pipes_{};
    std::array<OpenFileDescription, max_open_descriptions> descriptions_{};
    std::array<DescriptionPositionLock, max_open_descriptions> positions_{};
    u64 next_unix_receive_token_{1}; // ABI58 diagnostic name; shared by UNIX receive, TCP accept and fd installation.
    usize active_unix_receives_{}; // Shutdown admission waits for prepared resource owners.
    usize active_descriptor_reservations_{};
    bool collecting_unix_rights_{};
    net::NativeUdpService *native_udp_service_{};
    net::NativeTcpStreamService *native_tcp_service_{};
    user::native::SharedMemoryFiles *shared_memory_files_{};
    bool shutdown_admission_closed_{};
    ResourceDrainReport resource_drain_report_{};
    ProcessResources boot_resources_{};
    SchedulingMode mode_{SchedulingMode::round_robin};
    // Process slots never move or compact (task_table.hpp). Slot allocation
    // and release hold task_table, then the slot's task lock.
    TaskTableLock task_table_{};
    std::array<ProcessSlot, max_processes> slots_{};
    usize next_slot_{};
    friend struct SchedulerIdentityTestAccess;
    u64 next_task_id_{2};
    u64 next_address_space_id_{1};
    usize cpu_count_{1};
    std::array<CpuSlot, smp::max_cpus> cpu_slots_{};
    u32 online_cpus_{1};
    smp::PerCpu<SchedulerRunQueue> run_queues_{};
    std::array<AffinityBroken, 16> affinity_broken_{};
    u64 affinity_broken_total_{}, affinity_broken_taken_{};
    bool native_userland_{};
};

template <SchedulerPolicy Policy = RoundRobinPolicy> class BasicScheduler final : public SchedulerState
{
  public:
    BasicScheduler() = default;
    explicit BasicScheduler(Policy policy) : policy_(std::move(policy))
    {
    }

    void set_policy(Policy policy)
    {
        policy_ = std::move(policy);
    }
    [[nodiscard]] const Policy &policy() const
    {
        return policy_;
    }

    Result<ProcessId> schedule_next()
    {
        return schedule_next_on_cpu(0);
    }
    Result<ProcessId> schedule_next_on_cpu(smp::CpuId cpu)
    {
        if (shutdown_admission_closed()) return Status::denied("scheduler shutdown admission is closed");
        if (cpu >= cpu_count())
        {
            return Status::invalid_argument("scheduler cpu id out of range");
        }
        if (native_userland()) return schedule_kernel_thread_on_cpu(cpu);
        return dispatch_selected(cpu, policy_.pick_next(processes(), current_pid(cpu), cpu));
    }

  private:
    [[no_unique_address]] Policy policy_{};
};

using Scheduler = BasicScheduler<RoundRobinPolicy>;
static_assert(max_unix_endpoints == unix_pathname_endpoint_capacity);
static_assert(max_unix_backlog == unix_pathname_backlog_capacity);
static_assert(SchedulerPolicy<RoundRobinPolicy>);
static_assert(!std::is_polymorphic_v<Scheduler>);
static_assert(fs::pts_capacity == private_pty::RawPtyTable<>::pair_capacity,
              "every PTY pair slot has one /dev/pts entry");
static_assert(std::is_same_v<private_pty::RawPtyTable<>::SlaveTerminal, Terminal>,
              "every kernel PTY slave is a Terminal with the kernel line discipline");

} // namespace ok::sched
