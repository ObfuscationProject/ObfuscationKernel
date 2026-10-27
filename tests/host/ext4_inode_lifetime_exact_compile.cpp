#include "ok/fs/ext4/alloc.hpp"

ok::Status compile_lifetime_exact(ok::fs::Ext4InodeLifetimeTable<128, 512> &table,
                                  ok::fs::Ext4InodeLifetimeKey key)
{
    return table.schedule_exact(key, [&](const auto &ticket) -> ok::Status {
        return table.validate_dispatched(ticket, key);
    });
}
