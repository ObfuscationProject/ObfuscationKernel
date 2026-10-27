#include "ok/fs/ext4/txn.hpp"

ok::Status ext4_directory_replay_compile_probe(ok::driver::BlockDevice &device,
                                                 ok::fs::Ext4RecoveryBounds bounds)
{
    ok::fs::Ext4RecoveryCompletion<4, 2> recovery(false, true,
        ok::fs::Ext4ReplayPolicy::directory_rename);
    return recovery.run(device, bounds);
}
