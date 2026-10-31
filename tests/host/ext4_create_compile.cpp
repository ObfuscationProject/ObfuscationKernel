#include <ok/fs/ext4/mount.hpp>
#include <ok/core/module.hpp>

static_assert(ok::kernel_module_abi_version == 78);
template class ok::fs::Ext4MountedMetadata<4>;

using CreateOwner = ok::fs::Ext4OwnedTransaction<1, 8>;
ok::Status ext4_owned_create_compile_probe(CreateOwner &owner)
{
    const ok::fs::Ext4CreateUpdate update{2, 1700000111, 123456789, "empty-note",
        {ok::fs::NodeType::regular, 0600, 1000, 1001}};
    if (auto status = owner.prepare_create(update); !status) return status;
    if (!owner.created_inode()) return ok::Status::fault("create returned no inode identity");
    if (auto status = owner.commit(); !status) return status;
    return owner.checkpoint();
}
