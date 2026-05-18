#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <string_view>

namespace ok::user {

class UserModeGateway {
public:
    virtual ~UserModeGateway() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status enter(arch::UserEntry entry, arch::CpuContext& context) = 0;
};

class SimulatedUserModeGateway final : public UserModeGateway {
public:
    [[nodiscard]] std::string_view name() const override { return "simulated-user-mode-gateway"; }
    Status enter(arch::UserEntry entry, arch::CpuContext& context) override;
    [[nodiscard]] arch::UserEntry last_entry() const { return last_entry_; }

private:
    arch::UserEntry last_entry_ {};
};

class UserSpaceManager final {
public:
    explicit UserSpaceManager(UserModeGateway& gateway = default_gateway());

    static UserModeGateway& default_gateway();

    Status enter_process(sched::ProcessId pid, arch::UserEntry entry, arch::CpuContext& context);
    [[nodiscard]] sched::ProcessId last_entered_pid() const { return last_pid_; }

private:
    UserModeGateway* gateway_ {nullptr};
    sched::ProcessId last_pid_ {0};
};

} // namespace ok::user
