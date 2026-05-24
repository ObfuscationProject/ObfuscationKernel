#include "ok/user/user.hpp"

namespace ok::user
{

UserManager::UserManager()
{
    static_cast<void>(reset_defaults());
}

Status UserManager::reset_defaults()
{
    users_ = {};
    if (auto status = add_user(root_uid, root_gid, "kernel", "/", true); !status.ok())
    {
        return status;
    }
    if (auto status = add_user(root_uid, root_gid, "root", "/root", false); !status.ok())
    {
        return status;
    }
    return add_user(default_user_uid, default_user_gid, "user", "/home/user", false);
}

Status UserManager::add_user(UserId uid, GroupId gid, std::string_view name, std::string_view home, bool kernel_space)
{
    if (name.empty() || home.empty())
    {
        return Status::invalid_argument("user name and home directory are required");
    }
    if (find_by_name(name) != nullptr)
    {
        return Status::already_exists("user name already exists");
    }
    UserAccount account{.uid = uid, .gid = gid, .kernel_space = kernel_space};
    if (auto status = account.name.assign(name); !status.ok())
    {
        return status;
    }
    if (auto status = account.home.assign(home); !status.ok())
    {
        return status;
    }
    return users_.push_back(account);
}

const UserAccount *UserManager::find_by_name(std::string_view name) const
{
    for (const auto &account : users_)
    {
        if (account.name.view() == name)
        {
            return &account;
        }
    }
    return nullptr;
}

const UserAccount *UserManager::find_by_uid(UserId uid) const
{
    for (const auto &account : users_)
    {
        if (account.uid == uid && !account.kernel_space)
        {
            return &account;
        }
    }
    return nullptr;
}

const UserAccount *UserManager::find_by_gid(GroupId gid) const
{
    for (const auto &account : users_)
    {
        if (account.gid == gid && !account.kernel_space)
        {
            return &account;
        }
    }
    return nullptr;
}

Result<Credentials> UserManager::credentials_for(std::string_view name) const
{
    const auto *account = find_by_name(name);
    if (account == nullptr)
    {
        return Status::not_found("user account not found");
    }
    return Credentials{
        .uid = account->uid,
        .gid = account->gid,
        .euid = account->uid,
        .egid = account->gid,
        .kernel_space = account->kernel_space,
    };
}

bool UserManager::can_switch(Credentials current, Credentials target) const
{
    if (is_privileged(current))
    {
        return true;
    }
    return current.uid == target.uid && current.gid == target.gid;
}

Status SimulatedUserModeGateway::enter(arch::UserEntry entry, arch::CpuContext &context)
{
    if (entry.instruction_pointer == 0 || entry.stack_pointer == 0)
    {
        return Status::invalid_argument("invalid user entry point");
    }
    last_entry_ = entry;
    context.program_counter = entry.instruction_pointer;
    context.stack_pointer = entry.stack_pointer;
    context.registers[0] = entry.argument;
    context.mode = arch::PrivilegeMode::user;
    return Status::success();
}

UserSpaceManager::UserSpaceManager(UserModeGateway *gateway) : gateway_(gateway == nullptr ? &owned_gateway_ : gateway)
{
}

UserModeGateway &UserSpaceManager::default_gateway()
{
    static SimulatedUserModeGateway gateway;
    return gateway;
}

Status UserSpaceManager::enter_process(sched::ProcessId pid, arch::UserEntry entry, arch::CpuContext &context)
{
    if (pid == 0)
    {
        return Status::invalid_argument("invalid process id");
    }
    auto status = gateway_->enter(entry, context);
    if (!status.ok())
    {
        return status;
    }
    last_pid_ = pid;
    return Status::success();
}

Status UserSpaceManager::switch_credentials(Credentials &active, std::string_view name) const
{
    auto target = users_.credentials_for(name);
    if (!target)
    {
        return target.status();
    }
    if (!users_.can_switch(active, target.value()))
    {
        return Status::denied("user switch denied");
    }
    active = target.value();
    return Status::success();
}

} // namespace ok::user
