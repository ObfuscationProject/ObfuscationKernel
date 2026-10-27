#include <ok/fs/ext4/mount.hpp>
#include <type_traits>

using Lease = ok::fs::Ext4InodeLifetimeTable<128, 512>::Lease;
static_assert(!std::is_copy_constructible_v<Lease>);
static_assert(!std::is_copy_assignable_v<Lease>);
static_assert(std::is_move_constructible_v<Lease>);
static_assert(!std::is_move_assignable_v<Lease>);
template class ok::fs::Ext4MountedMetadata<4>;
