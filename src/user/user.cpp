#include "ok/user/user.hpp"

namespace ok::user {

Status SimulatedUserModeGateway::enter(arch::UserEntry entry, arch::CpuContext& context)
{
    if (entry.instruction_pointer == 0 || entry.stack_pointer == 0) {
        return Status::invalid_argument("invalid user entry point");
    }
    last_entry_ = entry;
    context.program_counter = entry.instruction_pointer;
    context.stack_pointer = entry.stack_pointer;
    context.registers[0] = entry.argument;
    context.mode = arch::PrivilegeMode::user;
    return Status::success();
}

UserSpaceManager::UserSpaceManager(std::unique_ptr<UserModeGateway> gateway)
    : gateway_(std::move(gateway))
{
}

Status UserSpaceManager::enter_process(sched::ProcessId pid, arch::UserEntry entry, arch::CpuContext& context)
{
    if (pid == 0) {
        return Status::invalid_argument("invalid process id");
    }
    auto status = gateway_->enter(entry, context);
    if (!status.ok()) {
        return status;
    }
    last_pid_ = pid;
    return Status::success();
}

} // namespace ok::user

