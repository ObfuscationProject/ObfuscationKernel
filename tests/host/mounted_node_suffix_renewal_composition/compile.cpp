// Explicit instantiation compiles private template bodies with normal access
// checking. It grants no public construction or mounted execution permission.
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <ok/fs/ext4_mounted_typed_cursor.hpp>
struct BridgeCompileAuthority {
    ok::Status validate(ok::fs::Ext4MountedTypedCursorPhase, ok::fs::Ext4MountedTypedSuffixStage) { return {}; }
    ok::Status complete_stage(ok::fs::Ext4MountedTypedSuffixStage) { return {}; }
};
using BridgeOwner = ok::fs::Ext4OwnedTransaction<2, 8>;
using BridgeSession = BridgeOwner::Registry::Session;
using BridgeCursor = ok::fs::Ext4MountedTypedCursor<BridgeSession, BridgeCompileAuthority>;
template class ok::fs::Ext4OwnedTransaction<2, 8>;
template class ok::fs::Ext4MountedMetadata<2>;
template class ok::fs::Ext4MountedTypedCursor<BridgeSession, BridgeCompileAuthority>;
template class BridgeOwner::TypedMountedCursorAttachment<BridgeCursor>;
template ok::Status BridgeOwner::typed_admit_mounted_cursor<BridgeCursor>(BridgeOwner::TypedMountedCursorAttachment<BridgeCursor> &);
template ok::Status BridgeOwner::typed_resume_mounted_cursor<BridgeCursor>(BridgeOwner::TypedMountedCursorAttachment<BridgeCursor> &);
template ok::Status BridgeOwner::typed_accept_mounted_cursor_receipt<BridgeCursor>(BridgeOwner::TypedMountedCursorAttachment<BridgeCursor> &, BridgeCursor::Receipt &);
using OriginalMount = ok::fs::Ext4MountedMetadata<2>;
struct OriginalNodeCompileOperation {
    template <typename Original, typename Authority>
    ok::Status operator()(const Original &, Authority &authority) const {
        return authority.validate(ok::fs::Ext4MountedTypedCursorPhase::unused,
                                  ok::fs::Ext4MountedTypedSuffixStage::none);
    }
};
template ok::Status OriginalMount::typed_node_mutation_scope<OriginalNodeCompileOperation>(
    OriginalMount::MountedNode &, ok::fs::Ext4MountedTypedOpening, OriginalNodeCompileOperation);
template ok::Status BridgeOwner::run_typed_zero_with_authority<OriginalMount::TypedOriginalNodeAuthority>(
    BridgeOwner::TypedOwnerWorkspace &, const ok::fs::Ext4RecoveryInfo &, ok::fs::Ext4ZeroTruncateUpdate,
    OriginalMount::TypedOriginalNodeAuthority &, bool);
template ok::Status BridgeOwner::run_typed_allocated_with_authority<OriginalMount::TypedOriginalNodeAuthority>(
    BridgeOwner::TypedOwnerWorkspace &, const ok::fs::Ext4RecoveryInfo &, ok::fs::Ext4AllocatedEmptyDataUpdate,
    ok::u32, const BridgeOwner::Block &, OriginalMount::TypedOriginalNodeAuthority &, bool);
template ok::Status BridgeOwner::typed_cancel_mounted_cursor_before_child<BridgeCursor>(
    BridgeOwner::TypedMountedCursorAttachment<BridgeCursor> &, ok::Status);
