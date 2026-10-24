#include "ok/fs/ext4_inode_lifetime.hpp"

ok::Status compile_lifetime_ticket(
    const ok::fs::Ext4InodeLifetimeTable<128, 512> &table,
    const ok::fs::Ext4InodeLifetimeTable<128, 512>::Ticket &ticket,
    ok::fs::Ext4InodeLifetimeKey key)
{ return table.validate_dispatched(ticket, key); }
