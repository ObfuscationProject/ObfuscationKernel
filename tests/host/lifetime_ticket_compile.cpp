#include "ok/fs/ext4/alloc.hpp"

ok::Status compile_lifetime_ticket(
    const ok::fs::Ext4InodeLifetimeTable<128, 512> &table,
    const ok::fs::Ext4InodeLifetimeTable<128, 512>::Ticket &ticket,
    ok::fs::Ext4InodeLifetimeKey key)
{ return table.validate_dispatched(ticket, key); }
