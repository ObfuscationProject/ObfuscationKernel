#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <string_view>

namespace ok::sched
{
using ProcessId = u64;
}

namespace ok::user
{

using UserId = u32;
using GroupId = u32;

inline constexpr usize max_users = 16;
inline constexpr UserId root_uid = 0;
inline constexpr GroupId root_gid = 0;
inline constexpr UserId default_user_uid = 1000;
inline constexpr GroupId default_user_gid = 1000;

struct Credentials
{
    UserId uid{root_uid};
    GroupId gid{root_gid};
    UserId euid{root_uid};
    GroupId egid{root_gid};
    bool kernel_space{false};
};

[[nodiscard]] constexpr Credentials kernel_credentials()
{
    return Credentials{.uid = root_uid, .gid = root_gid, .euid = root_uid, .egid = root_gid, .kernel_space = true};
}

[[nodiscard]] constexpr Credentials root_credentials()
{
    return Credentials{.uid = root_uid, .gid = root_gid, .euid = root_uid, .egid = root_gid, .kernel_space = false};
}

[[nodiscard]] constexpr Credentials default_user_credentials()
{
    return Credentials{.uid = default_user_uid,
                       .gid = default_user_gid,
                       .euid = default_user_uid,
                       .egid = default_user_gid,
                       .kernel_space = false};
}

[[nodiscard]] constexpr bool is_privileged(Credentials credentials)
{
    return credentials.kernel_space || credentials.euid == root_uid;
}

struct UserAccount
{
    UserId uid{root_uid};
    GroupId gid{root_gid};
    bool kernel_space{false};
    FixedString<32> name{};
    FixedString<64> home{};
};

class UserManager final
{
  public:
    UserManager();

    Status reset_defaults();
    Status add_user(UserId uid, GroupId gid, std::string_view name, std::string_view home, bool kernel_space = false);
    [[nodiscard]] const UserAccount *find_by_name(std::string_view name) const;
    [[nodiscard]] const UserAccount *find_by_uid(UserId uid) const;
    [[nodiscard]] const UserAccount *find_by_gid(GroupId gid) const;
    [[nodiscard]] Result<Credentials> credentials_for(std::string_view name) const;
    [[nodiscard]] bool can_switch(Credentials current, Credentials target) const;
    [[nodiscard]] usize user_count() const
    {
        return users_.size();
    }

  private:
    StaticVector<UserAccount, max_users> users_{};
};

enum class TransitionMode : u8
{
    simulated,
    trap_return,
    fast_return,
};

class UserModeGateway
{
  public:
    virtual ~UserModeGateway() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status enter(arch::UserEntry entry, arch::CpuContext &context) = 0;
};

class SimulatedUserModeGateway final : public UserModeGateway
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "simulated-user-mode-gateway";
    }
    Status enter(arch::UserEntry entry, arch::CpuContext &context) override;
    [[nodiscard]] arch::UserEntry last_entry() const
    {
        return last_entry_;
    }

  private:
    arch::UserEntry last_entry_{};
};

class UserSpaceManager final
{
  public:
    explicit UserSpaceManager(UserModeGateway *gateway = nullptr);

    static UserModeGateway &default_gateway();

    void set_mode(TransitionMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] TransitionMode mode() const
    {
        return mode_;
    }
    Status enter_process(sched::ProcessId pid, arch::UserEntry entry, arch::CpuContext &context);
    [[nodiscard]] sched::ProcessId last_entered_pid() const
    {
        return last_pid_;
    }
    [[nodiscard]] UserManager &users()
    {
        return users_;
    }
    [[nodiscard]] const UserManager &users() const
    {
        return users_;
    }
    [[nodiscard]] Result<Credentials> credentials_for(std::string_view name) const
    {
        return users_.credentials_for(name);
    }
    Status switch_credentials(Credentials &active, std::string_view name) const;

  private:
    TransitionMode mode_{TransitionMode::simulated};
    SimulatedUserModeGateway owned_gateway_{};
    UserModeGateway *gateway_{nullptr};
    sched::ProcessId last_pid_{0};
    UserManager users_{};
};

} // namespace ok::user
