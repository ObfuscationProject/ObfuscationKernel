#include <ok/fs/ext4/mount.hpp>
#include <ok/core/module.hpp>
static_assert(ok::kernel_module_abi_version == 76);
template class ok::fs::Ext4MountedMetadata<4>;
using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;
ok::Status first_data_compile_probe(Owner &owner, const std::array<ok::u8, 1024> &image)
{
    if (auto status = owner.prepare_first_data({14, 23, 1700000300, 234567891}, 7, image); !status) return status;
    if (auto status = owner.commit(); !status) return status;
    return owner.checkpoint();
}
