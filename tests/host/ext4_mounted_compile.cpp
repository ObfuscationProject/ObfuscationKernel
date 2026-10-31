#include <ok/fs/ext4/mount.hpp>
#include <ok/core/module.hpp>
static_assert(ok::kernel_module_abi_version == 78);
template class ok::fs::Ext4MountedMetadata<4>;
