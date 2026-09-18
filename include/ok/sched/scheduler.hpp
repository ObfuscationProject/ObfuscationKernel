#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <concepts>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ok::sched
{

using ProcessId = u64;
using ThreadId = u64;
inline constexpr usize max_processes = 64;
inline constexpr usize max_threads_per_process = 8;
inline constexpr usize max_process_name = 32;
inline constexpr u8 scheduler_min_priority = 0;
inline constexpr u8 scheduler_default_priority = 16;
inline constexpr u8 scheduler_max_priority = 31;
inline constexpr u16 cpu_affinity_any = 0xffffu;

enum class ProcessState : u8
{
    created,
    runnable,
    running,
    blocked,
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

    [[nodiscard]] ProcessId pid() const
    {
        return pid_;
    }
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
    ProcessId pid_{0};
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
    SchedulerState() = default;
    ~SchedulerState() = default;
    Result<ProcessId> dispatch_selected(smp::CpuId cpu, Result<ProcessId> next);

  public:
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
    Result<ProcessId> create_background_process(std::string_view name, arch::CpuContext initial_context);
    Result<ThreadId> create_thread(ProcessId pid, arch::CpuContext initial_context);
    Result<ProcessId> spawn(ScheduleRequest request);
    Status set_credentials(ProcessId pid, user::Credentials credentials);
    Status set_priority(ProcessId pid, u8 priority);
    Status set_cpu_affinity(ProcessId pid, u16 cpu_affinity_mask);
    Status set_cpu_accounting(ProcessId pid, ProcessCpuAccounting accounting);
    Status kill_process(ProcessId pid);
    Status configure_cpus(usize cpu_count);
    Status set_runnable(ProcessId pid);
    Status block_process(ProcessId pid);

    [[nodiscard]] ProcessId current_pid() const
    {
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
    [[nodiscard]] Result<ThreadId> pick_next_thread(ProcessControlBlock &process, ThreadId current, smp::CpuId cpu);
    [[nodiscard]] u16 configured_cpu_mask() const;

    SchedulingMode mode_{SchedulingMode::round_robin};
    StaticVector<ProcessControlBlock, max_processes> processes_{};
    ProcessId next_pid_{1};
    ThreadId next_tid_{1};
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
static_assert(SchedulerPolicy<RoundRobinPolicy>);
static_assert(!std::is_polymorphic_v<Scheduler>);

} // namespace ok::sched
