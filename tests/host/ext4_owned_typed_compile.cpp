#include <ok/fs/ext4/mount.hpp>

// Instantiate the actual production owner, including private typed entry points.
// These host frames do not qualify a target call chain or mounted enablement.
template class ok::fs::Ext4OwnedTransaction<2, 8>;
template ok::Status ok::fs::Ext4OwnedTransaction<2, 32>::run_typed_zero(
    ok::fs::Ext4OwnedTransaction<2, 32>::TypedOwnerWorkspace &,
    const ok::fs::Ext4RecoveryInfo &, ok::fs::Ext4ZeroTruncateUpdate, bool);
template ok::Status ok::fs::Ext4OwnedTransaction<2, 32>::run_typed_allocated(
    ok::fs::Ext4OwnedTransaction<2, 32>::TypedOwnerWorkspace &,
    const ok::fs::Ext4RecoveryInfo &, ok::fs::Ext4AllocatedEmptyDataUpdate, ok::u32,
    const std::array<ok::u8, 1024> &, bool);
