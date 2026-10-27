#include <ok/fs/ext4/mount.hpp>
using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;
ok::Status namespace_cold_prepare(Owner &owner) { return owner.prepare_namespace_recovery(); }
ok::Status namespace_cold_recover(Owner &owner) { return owner.recover_namespace(); }
ok::Status namespace_live_checkpoint(Owner &owner) { return owner.checkpoint(); }
