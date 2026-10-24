#include "../fixtures/ext4_rename_relocation_fixture.hpp"
#include <ok/fs/ext4_completion.hpp>

ok::Status relocation_compile_probe(ok::driver::BlockDevice &device, ok::fs::Ext4RecoveryBounds bounds)
{
    ok::testing::rename_fixture::Fixture fixture;
    const auto plan = fixture.plan();
    if (!plan) return plan.status();
    ok::fs::Ext4RecoveryCompletion<4, 2> completion(false, true, ok::fs::Ext4ReplayPolicy::directory_rename);
    return completion.run(device, bounds);
}
