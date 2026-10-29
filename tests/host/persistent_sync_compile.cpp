#include <ok/fs/ext4/mount.hpp>
#include <ok/core/module.hpp>
#include <ok/uapi/syscall.h>
static_assert(ok::kernel_module_abi_version == 77);
static_assert(OK_UAPI_VERSION_MINOR == 16 && OK_SYS_SYNC_PERSISTENT == 1031);
// Match the production one-device, nine-buffer owner specialization.
template class ok::fs::Ext4MountedMetadata<1>;
