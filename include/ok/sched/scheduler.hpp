#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/sched/resources.hpp"
#include "ok/sched/unix_rights.hpp"
#include "ok/sched/unix_pathname_sdk_profile.hpp"
#include "ok/sched/descriptor_reservation.hpp"
#include "ok/sched/pty_pair.hpp"
#include "ok/sched/signal_mask.hpp"
#include "ok/sched/deadline.hpp"
#include "ok/sched/console_input.hpp"
#include "ok/sched/console_control.hpp"
#include "ok/sched/wait_registration.hpp"
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

using ProcessId = u64;
using ThreadId = u64;
inline constexpr ProcessId user_init_pid = 1;
// Common Linux-visible task namespace; zero is reserved for boot/service context.
// Linux robust futex owner words reserve the upper two bits for state flags.
inline constexpr u64 max_task_id = 0x3fffffffU;
inline constexpr usize max_processes = 64;
inline constexpr usize max_threads_per_process = 8;
inline constexpr usize max_process_name = 32;
inline constexpr u8 scheduler_min_priority = 0;
inline constexpr u8 scheduler_default_priority = 16;
inline constexpr u8 scheduler_max_priority = 31;
inline constexpr u16 cpu_affinity_any = 0xffffu;

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
    [[nodiscard]] StaticVector<ThreadControlBlock, max_threads_per_process> &threads()
    {
        return threads_;
    }
    [[nodiscard]] const StaticVector<ThreadControlBlock, max_threads_per_process> &threads() const
    {
        return threads_;
    }

  private:
    friend class SchedulerState;
    ProcessId pid_{0};
    u32 console_epoch_{}; // Only the scheduler may issue or inherit membership.
    ProcessId parent_pid_{0};
    ProcessGroup group_{};
    Session session_{};
    bool has_execed_{}; // Successful image replacement since this process was forked.
    SignalState signals_{};
    ProcessResources resources_{};
    StaticVector<ProcessId, max_process_children> children_{};
    i32 exit_code_{0};
    u8 termination_signal_{0};
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
    StaticVector<ThreadControlBlock, max_threads_per_process> threads_{};
};

// Policies are kernel build-time choices. The scheduler stores its policy by
// value so copies cannot retain a pointer into a different scheduler instance.
template <typename T>
concept SchedulerPolicy =
    requires(T &policy, std::span<const ProcessControlBlock> processes, ProcessId current, smp::CpuId cpu) {
        { policy.pick_next(processes, current, cpu) } -> std::same_as<Result<ProcessId>>;
    };

class RoundRobinPolicy final
{
  public:
    [[nodiscard]] constexpr std::string_view name() const
    {
        return "round-robin";
    }
    Result<ProcessId> pick_next(std::span<const ProcessControlBlock> processes, ProcessId current, smp::CpuId cpu);
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
        bool live = false;
        for (const auto &thread : process->threads()) live |= thread.state != ProcessState::exited;
        if (!live) return Status::invalid_argument("cannot create a thread in a retired group");
        ThreadControlBlock *retired = nullptr;
        for (auto &thread : process->threads())
            if (thread.state == ProcessState::exited && thread.tid != pid && !thread.native_active &&
                !thread.wait_registration.active() && !thread.entry_stack.pool && !thread.kernel_stack_top)
            {
                retired = &thread;
                break;
            }
        if (!retired && process->threads().full())
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
        if (retired)
            *retired = created;
        else if (auto status = process->threads().push_back(created); !status.ok())
            return status;
        if (process->state() == ProcessState::created || process->state() == ProcessState::blocked)
        {
            process->set_state(ProcessState::runnable);
        }
        return tid;
    }

    // Serialized retirement only; caller drains external waits and releases
    // thread-owned resources first. Process/VM/FD ownership remains unchanged.
    Result<usize> exit_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu = 0);
    Result<ProcessId> spawn(ScheduleRequest request);
    Status set_credentials(ProcessId pid, user::Credentials credentials);
    Status set_priority(ProcessId pid, u8 priority);
    Status set_cpu_affinity(ProcessId pid, u16 cpu_affinity_mask);
    Status set_cpu_accounting(ProcessId pid, ProcessCpuAccounting accounting);
    Status kill_process(ProcessId pid);
    Status activate(ProcessId pid, smp::CpuId cpu = 0);
    Status activate_thread(ProcessId pid, ThreadId tid, smp::CpuId cpu = 0);
    Result<ProcessId> fork_process(ProcessId parent, bool publish = true);
    // Trusted serialized process mechanisms. Native and Linux syscall boundaries
    // authenticate the caller before entering these operations.
    [[nodiscard]] Result<ProcessId> process_group(ProcessId caller, i32 pid) const;
    [[nodiscard]] Result<ProcessId> session_id(ProcessId caller, i32 pid) const;
    Status set_process_group(ProcessId caller, i32 pid, i32 pgid);
    [[nodiscard]] Result<ProcessId> create_session(ProcessId caller);
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
    Result<PreparedPtyRead> prepare_pty_read(u16 description, usize count);
    Result<usize> commit_pty_read(PreparedPtyRead &preview);
    // Keys may spuriously collide across pair generations. Retained handle and
    // wait-ticket validation must compare the complete generation separately.
    static constexpr u64 pty_wait_key(private_pty::Handle endpoint)
    { return (static_cast<u64>(endpoint.slot) << 1) | (endpoint.side == private_pty::Side::slave ? 1u : 0u); }
    Result<usize> read_pty_description(u16 description, std::span<std::byte> output);
    Result<usize> write_pty_description(u16 description, std::span<const std::byte> input);
    Result<private_pty::Readiness> pty_description_readiness(u16 description) const;
    [[nodiscard]] private_pty::Audit pty_audit() const { return pty_transport_.audit(); }
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
    Result<i64> console_control(ConsoleControlOperation operation, i32 value = 0);
    [[nodiscard]] bool console_readable(ProcessId pid, u32 expected_epoch) const;
    [[nodiscard]] Status shutdown_console_status() const;
    Result<usize> read_console(ProcessId pid, u32 expected_epoch, std::span<std::byte> output);
    // `source` is the console's: interrupt once its UART line is bound.
    void console_input(char value, WakeSource source = WakeSource::thread);
    // Trusted runtime ingress failure closes ownership before returning to userland.
    void abort_console_input();
    Status configure_cpus(usize cpu_count);
    Status set_runnable(ProcessId pid);
    Status block_process(ProcessId pid);

    [[nodiscard]] ProcessId current_pid() const
    {
        // No-argument native services operate on the serialized boot CPU.
        return current_pid_;
    }
    [[nodiscard]] ProcessId current_pid(smp::CpuId cpu) const;
    [[nodiscard]] ThreadId current_tid(smp::CpuId cpu) const;
    [[nodiscard]] usize cpu_count() const
    {
        return cpu_count_;
    }
    [[nodiscard]] usize process_count() const
    {
        return processes_.size();
    }
    [[nodiscard]] usize background_process_count() const;
    [[nodiscard]] ProcessControlBlock *find(ProcessId pid);
    [[nodiscard]] const ProcessControlBlock *find(ProcessId pid) const;
    [[nodiscard]] std::span<ProcessControlBlock> processes()
    {
        return {processes_.begin(), processes_.size()};
    }
    [[nodiscard]] std::span<const ProcessControlBlock> processes() const
    {
        return {processes_.begin(), processes_.size()};
    }
    [[nodiscard]] CpuSchedulingStats cpu_stats(smp::CpuId cpu) const;
    [[nodiscard]] u64 total_dispatches() const;
    [[nodiscard]] u64 total_accounted_dispatches() const;
    [[nodiscard]] u8 cpu_usage_percent(smp::CpuId cpu) const;
    [[nodiscard]] u8 process_usage_percent(ProcessId pid) const;

  private:
    [[nodiscard]] bool thread_running_on_other_cpu(ThreadId tid, smp::CpuId cpu) const;
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
    [[nodiscard]] ProcessControlBlock *console_caller();
    void retire_console_process(ProcessId pid);
    void cancel_console_epoch(u32 epoch);
    private_pty::RawPtyTable<> pty_transport_{}; // One stable table; handles never cross scheduler domains.
    ConsoleInputLease console_input_{};
    std::array<u64, wake_source_count> wakes_{};
    struct ConsoleTerminal
    {
        u32 epoch{};
        ProcessId session{}, foreground{};
    } console_terminal_{};
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
    u64 next_unix_receive_token_{1}; // ABI58 diagnostic name; shared by UNIX receive, TCP accept and fd installation.
    usize active_unix_receives_{}; // Process compaction cannot move a prepared resource owner.
    usize active_descriptor_reservations_{};
    bool collecting_unix_rights_{};
    net::NativeUdpService *native_udp_service_{};
    net::NativeTcpStreamService *native_tcp_service_{};
    user::native::SharedMemoryFiles *shared_memory_files_{};
    bool shutdown_admission_closed_{};
    ResourceDrainReport resource_drain_report_{};
    ProcessResources boot_resources_{};
    SchedulingMode mode_{SchedulingMode::round_robin};
    StaticVector<ProcessControlBlock, max_processes> processes_{};
    friend struct SchedulerIdentityTestAccess;
    u64 next_task_id_{2};
    u64 next_address_space_id_{1};
    ProcessId current_pid_{0};
    usize cpu_count_{1};
    std::array<ProcessId, smp::max_cpus> current_by_cpu_{};
    std::array<ThreadId, smp::max_cpus> current_thread_by_cpu_{};
    std::array<CpuSchedulingStats, smp::max_cpus> cpu_stats_{};
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

} // namespace ok::sched
