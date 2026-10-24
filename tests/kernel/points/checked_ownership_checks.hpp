#pragma once

#include "ok/core/kernel.hpp"
#include "ok/fs/mode_policy.hpp"

namespace ok::tests
{
// Boot policy/refusal model; actual mounted transactions run in hosted tests.
inline Status check_checked_ownership_model(Kernel &kernel)
{
    const auto root = user::root_credentials();
    const auto preserved = fs::owner_change_policy(1000, 1001, 0107754, ~u32{0}, ~u32{0}, root);
    const auto owner = fs::owner_change_policy(1000, 1001, 0107754, 2000, ~u32{0}, root);
    const auto group = fs::owner_change_policy(1000, 1001, 0047754, ~u32{0}, 2001, root);
    const auto unchanged = fs::owner_change_policy(1000, 1001, 0107754, 1000, 1001, root);
    const auto denied = fs::owner_change_policy(1000, 1001, 0107754, ~u32{0}, ~u32{0},
        {.uid = 0, .gid = 0, .euid = 1000, .egid = 1001});
    if (!preserved.authorized || preserved.uid != 1000 || preserved.gid != 1001 || preserved.permissions != 07754 ||
        !owner.authorized || owner.uid != 2000 || owner.gid != 1001 || owner.permissions != 01754 ||
        !group.authorized || group.uid != 1000 || group.gid != 2001 || group.permissions != 01754 ||
        !unchanged.authorized || unchanged.permissions != 07754 || denied.authorized)
        return Status::fault("checked ownership root/sentinel/privilege-bit policy model failed");
    fs::RamNode unsupported;
    if (unsupported.change_owner(1, 2, root).kind() != fs::ModeChangeResult::Kind::unavailable ||
        kernel.posix().change_owner_fd(-1, 1, 2).status().code() != StatusCode::bad_descriptor ||
        kernel.posix().change_owner_at(posix::at_FDCWD, "/", 1, 2, 0x80000000).status().code() != StatusCode::invalid_argument)
        return Status::fault("checked ownership provider/fd/flag refusal model failed");
    return {};
}
} // namespace ok::tests
