#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ok::sched {

using ProcessId = u64;
using ThreadId = u64;

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
    ProcessControlBlock(ProcessId pid, std::string name);

    [[nodiscard]] ProcessId pid() const { return pid_; }
    [[nodiscard]] std::string_view name() const { return name_; }
    [[nodiscard]] ProcessState state() const { return state_; }
    void set_state(ProcessState state) { state_ = state; }
    [[nodiscard]] std::vector<ThreadControlBlock>& threads() { return threads_; }
    [[nodiscard]] const std::vector<ThreadControlBlock>& threads() const { return threads_; }

private:
    ProcessId pid_;
    std::string name_;
    ProcessState state_ {ProcessState::created};
    std::vector<ThreadControlBlock> threads_;
};

class SchedulerPolicy {
public:
    virtual ~SchedulerPolicy() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Result<ProcessId> pick_next(std::span<const std::shared_ptr<ProcessControlBlock>> processes,
                                        ProcessId current) = 0;
};

class RoundRobinPolicy final : public SchedulerPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "round-robin"; }
    Result<ProcessId> pick_next(std::span<const std::shared_ptr<ProcessControlBlock>> processes,
                                ProcessId current) override;
};

class Scheduler final {
public:
    explicit Scheduler(std::unique_ptr<SchedulerPolicy> policy = std::make_unique<RoundRobinPolicy>());

    Result<ProcessId> create_process(std::string name, arch::CpuContext initial_context);
    Status set_runnable(ProcessId pid);
    Result<ProcessId> schedule_next();

    [[nodiscard]] ProcessId current_pid() const { return current_pid_; }
    [[nodiscard]] usize process_count() const { return processes_.size(); }
    [[nodiscard]] ProcessControlBlock* find(ProcessId pid);
    [[nodiscard]] const ProcessControlBlock* find(ProcessId pid) const;

private:
    std::unique_ptr<SchedulerPolicy> policy_;
    std::vector<std::shared_ptr<ProcessControlBlock>> processes_;
    ProcessId next_pid_ {1};
    ThreadId next_tid_ {1};
    ProcessId current_pid_ {0};
};

} // namespace ok::sched

