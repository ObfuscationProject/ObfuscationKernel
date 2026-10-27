#include "ok/core/module.hpp"
#include "ok/fs/ext4/mount.hpp"
#include "ok/sched/scheduler.hpp"
#include <type_traits>
static_assert(ok::kernel_module_abi_version == 52);
static_assert(!std::is_copy_constructible_v<ok::fs::MountedRootReference>);
static_assert(!std::is_move_assignable_v<ok::fs::MountedRootReference>);
template class ok::fs::Ext4MountedMetadata<1>;
ok::Status mount_retention_probe(ok::fs::Ext4MountedMetadata<1> &provider,
    ok::fs::VirtualFileSystem &vfs, ok::sched::Scheduler &scheduler)
{
    auto root = provider.retain_root();
    if (!root) return root.status();
    auto attached = vfs.mount_owned("/", std::move(root.value()));
    if (!attached) return attached.status();
    if (auto status = vfs.detach(attached.value()); !status) return status;
    if (auto status = scheduler.drain_all_resources(); !status) return status;
    if (auto status = provider.begin_drain(); !status) return status;
    return provider.freeze_clean();
}
