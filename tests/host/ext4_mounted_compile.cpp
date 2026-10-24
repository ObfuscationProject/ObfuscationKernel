#include <ok/fs/ext4_mounted_metadata.hpp>
#include <ok/core/module.hpp>
static_assert(ok::kernel_module_abi_version == 52);
template class ok::fs::Ext4MountedMetadata<4>;
