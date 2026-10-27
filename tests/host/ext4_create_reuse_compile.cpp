#include <ok/fs/ext4/mount.hpp>

extern "C" ok::Status compile_create_reuse(ok::fs::Ext4OwnedTransaction<1, 8> &owner,
    const ok::fs::Ext4CreateUpdate &update)
{
    if (auto status = owner.prepare_create(update); !status) return status;
    if (auto status = owner.commit(); !status) return status;
    return owner.checkpoint();
}
