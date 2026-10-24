#include <ok/core/module.hpp>
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <cstdio>

using namespace ok;
int main()
{
    std::printf("abi=%u pointer=%zu manifest=%zu/%zu abi_offset=%zu resources_offset=%zu image=%zu/%zu services=%zu/%zu module=%zu/%zu manager=%zu/%zu mounted=%zu/%zu owned8=%zu/%zu\n",
        kernel_module_abi_version, sizeof(void *), sizeof(ModuleManifest), alignof(ModuleManifest),
        offsetof(ModuleManifest, abi_version), offsetof(ModuleManifest, resources),
        sizeof(ModuleImageInfo), alignof(ModuleImageInfo), sizeof(ServiceRegistry), alignof(ServiceRegistry),
        sizeof(KernelModule), alignof(KernelModule), sizeof(ModuleManager), alignof(ModuleManager),
        sizeof(fs::Ext4MountedMetadata<4>), alignof(fs::Ext4MountedMetadata<4>),
        sizeof(fs::Ext4OwnedTransaction<4, 8>), alignof(fs::Ext4OwnedTransaction<4, 8>));
}
