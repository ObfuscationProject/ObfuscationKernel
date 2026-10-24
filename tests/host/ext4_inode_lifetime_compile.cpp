#include "ok/fs/ext4_inode_lifetime.hpp"

ok::Status compile_ext4_inode_lifetime(ok::fs::Ext4InodeLifetimeTable<> &table,
                                       ok::fs::Ext4InodeLifetimeKey key)
{
    if (auto status = table.observe(key); !status) return status;
    auto hold = table.retain(key, ok::fs::Ext4InodeHold::descriptor);
    if (!hold) return hold.status();
    if (auto status = table.mark_orphan(key); !status) return status;
    return table.release(hold.value());
}
