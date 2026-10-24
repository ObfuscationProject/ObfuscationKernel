#include <ok/fs/ext4_namespace_recovery_binding.hpp>

ok::Status compile_namespace_recovery(ok::fs::Ext4NamespaceRecoveryAdmission &admission,
    ok::driver::BlockDevice &device, ok::fs::Ext4RecoveryBounds bounds,
    std::span<std::byte> old_sector)
{
    auto status = admission.inspect(device, bounds);
    if (!status || !admission.info() || !admission.result() || !admission.scratch()) return status;
    if (!admission.count() || !admission.before(0) || !admission.after(0))
        return ok::Status::fault("namespace recovery incomplete compile API");
    return admission.read_old_sectors(ok::u64{admission.target(0)} * 2, old_sector);
}
