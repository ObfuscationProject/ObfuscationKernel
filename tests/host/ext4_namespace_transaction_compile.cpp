#include <ok/fs/ext4_owned_transaction.hpp>

using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;

ok::Status compile_namespace_prepare(Owner &owner, ok::fs::Ext4NamespaceUpdate update)
{ return owner.prepare_namespace(update); }

ok::Status compile_namespace_commit(Owner &owner)
{ return owner.commit(); }

ok::Status compile_namespace_checkpoint(Owner &owner)
{ return owner.checkpoint(); }
