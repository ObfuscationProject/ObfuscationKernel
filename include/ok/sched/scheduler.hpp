#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::sched {

using ProcessId = u64;
using ThreadId = u64;
inline constexpr usize max_processes = 64;
inline constexpr usize max_threads_per_process = 8;
inline constexpr usize max_process_name = 32;

enum class ProcessState : u8 {
    created,
    runnable,
    running,
    blocked,
    exited,
};

struct ThreadControlBlock {
    ThreadId tid {0};
    ProcessId owner {0};
    ProcessState state {ProcessState::created};
    arch::CpuContext context {};
};

class ProcessControlBlock {
public:
    ProcessControlBlock() = default;
    ProcessControlBlock(ProcessId pid, std::string_view name);

    [[nodiscard]] ProcessId pid() const { return pid_; }
    [[nodiscard]] std::string_view name() const { return name_.view(); }
    [[nodiscard]] ProcessState state() const { return state_; }
    void set_state(ProcessState state) { state_ = state; }
    [[nodiscard]] StaticVector<ThreadControlBlock, max_threads_per_process>& threads() { return threads_; }
    [[nodiscard]] const StaticVector<ThreadControlBlock, max_threads_per_process>& threads() const { return threads_; }

private:
    ProcessId pid_ {0};
    FixedString<max_process_name> name_ {};
    ProcessState state_ {ProcessState::created};
    StaticVector<ThreadControlBlock, max_threads_per_process> threads_ {};
};

class SchedulerPolicy {
public:
    virtual ~SchedulerPolicy() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Result<ProcessId> pick_next(std::span<const ProcessControlBlock> processes, ProcessId current) = 0;
};

class RoundRobinPolicy final : public SchedulerPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "round-robin"; }
    Result<ProcessId> pick_next(std::span<const ProcessControlBlock> processes, ProcessId current) override;
};

class Scheduler final {
public:
    explicit Scheduler(SchedulerPolicy& policy = default_round_robin_policy());

    static SchedulerPolicy& default_round_robin_policy();

    Result<ProcessId> create_process(std::string_view name, arch::CpuContext initial_context);
    Status configure_cpus(usize cpu_count);
    Status set_runnable(ProcessId pid);
    Result<ProcessId> schedule_next();
    Result<ProcessId> schedule_next_on_cpu(smp::CpuId cpu);

    [[nodiscard]] ProcessId current_pid() const { return current_pid_; }
    [[nodiscard]] ProcessId current_pid(smp::CpuId cpu) const;
    [[nodiscard]] usize cpu_count() const { return cpu_count_; }
    [[nodiscard]] usize process_count() const { return processes_.size(); }
    [[nodiscard]] ProcessControlBlock* find(ProcessId pid);
    [[nodiscard]] const ProcessControlBlock* find(ProcessId pid) const;

private:
    SchedulerPolicy* policy_ {nullptr};
    StaticVector<ProcessControlBlock, max_processes> processes_ {};
    ProcessId next_pid_ {1};
    ThreadId next_tid_ {1};
    ProcessId current_pid_ {0};
    usize cpu_count_ {1};
    std::array<ProcessId, smp::max_cpus> current_by_cpu_ {};
};

} // namespace ok::sched
