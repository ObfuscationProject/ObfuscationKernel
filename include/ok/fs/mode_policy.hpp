#pragma once

#include "ok/core/types.hpp"
#include "ok/user/user.hpp"

namespace ok::fs
{

struct ModePolicyDecision
{
    bool authorized{};
    u32 permissions{};
};

// The current flat credential domain has no supplementary groups or fsuid.
// Provider/type admission and the checked metadata commit surround this policy.
[[nodiscard]] constexpr ModePolicyDecision mode_change_policy(
    u32 inode_owner, u32 inode_group, u32 requested, user::Credentials caller)
{
    if (caller.euid != user::root_uid && caller.euid != inode_owner)
        return {};
    u32 permissions = requested & 07777u;
    if (caller.euid != user::root_uid && caller.egid != inode_group)
        permissions &= ~02000u;
    return {.authorized = true, .permissions = permissions};
}

struct OwnerPolicyDecision
{
    bool authorized{};
    u32 uid{}, gid{}, permissions{};
};

// Root alone may change ownership in the initial flat credential domain.
// Each all-ones operand independently preserves its existing identifier.
[[nodiscard]] constexpr OwnerPolicyDecision owner_change_policy(
    u32 owner, u32 group, u32 mode, u32 uid, u32 gid, user::Credentials caller)
{
    if (caller.euid != user::root_uid) return {};
    const u32 next_uid = uid == ~u32{0} ? owner : uid;
    const u32 next_gid = gid == ~u32{0} ? group : gid;
    u32 permissions = mode & 07777u;
    if (next_uid != owner || next_gid != group) permissions &= ~06000u;
    return {.authorized = true, .uid = next_uid, .gid = next_gid, .permissions = permissions};
}

} // namespace ok::fs
