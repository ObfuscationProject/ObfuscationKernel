#include "ok/fs/ext4_owned_transaction.hpp"

using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;
ok::Status ext4_owned_rename_compile_probe(Owner &owner, Owner::Registry &registry,
    Owner::Registry::Registration key, ok::block::SessionBounds bounds)
{
    if (auto status = owner.open(registry, key, bounds); !status) return status;
    const ok::fs::Ext4RenameUpdate update{2, 12, 1700000111, 123456789, "seed", "note"};
    if (auto status = owner.prepare_rename(update); !status) return status;
    if (auto status = owner.commit(); !status) return status;
    if (auto status = owner.checkpoint(); !status) return status;
    return owner.close();
}
