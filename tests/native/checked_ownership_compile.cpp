#include "ok/fs/ext4/mount.hpp"
#include "ok/posix/posix.hpp"

ok::fs::ModeChangeResult checked_ownership_compile(ok::fs::Node &node, ok::posix::PosixService &service,
    ok::u32 uid, ok::u32 gid, ok::user::Credentials caller)
{
    const auto changed = node.change_owner(uid, gid, caller);
    if (!changed.ok()) return changed;
    return service.change_owner_at(-100, "/home/user", uid, gid);
}
template class ok::fs::Ext4MountedMetadata<1>;
