// Freestanding instantiation only; this does not execute a mount or a guest.
#include <ok/fs/ext4/mount.hpp>
template class ok::fs::Ext4MountedMetadata<1>;
static_assert(!std::is_copy_constructible_v<ok::fs::Ext4MountedReclaimWorkspace>);
static_assert(!std::is_move_constructible_v<ok::fs::Ext4MountedReclaimWorkspace>);
