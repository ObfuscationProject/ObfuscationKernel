#include "ok/fs/ext4_recovery.hpp"

ok::Status ext4_recovery_compile_probe(ok::driver::BlockDevice &device, ok::fs::Ext4RecoveryBinding &binding,
                                        ok::fs::Ext4RecoveryBounds bounds)
{
    const auto status = binding.inspect(device, bounds);
    if (!status)
        return status;
    const auto *facts = binding.info();
    if (!facts || !facts->structural_metadata().valid())
        return ok::Status::fault("admitted ext4 binding lacks its metadata domain");
    return ok::Status::success();
}
