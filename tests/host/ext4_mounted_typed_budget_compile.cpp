#include <ok/fs/ext4_mounted_typed_budget.hpp>

ok::Result<ok::fs::Ext4MountedTypedCapacity> compile_mounted_typed_budget(
    const ok::block::SessionReport &report, ok::u32 groups, ok::u64 blocks,
    ok::fs::Ext4MountedTypedOrigin origin, ok::fs::Ext4MountedTypedRoute route)
{
    return ok::fs::Ext4MountedTypedBudget::capacity(report, groups, blocks, origin, route);
}
ok::Result<ok::u32> compile_mounted_typed_sequence(ok::fs::Ext4MountedTypedRoute route, ok::u32 sequence)
{ return ok::fs::Ext4MountedTypedBudget::final_sequence(route, sequence); }
