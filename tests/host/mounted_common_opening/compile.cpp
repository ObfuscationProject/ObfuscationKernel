// Normal-access freestanding common opening instantiation; no execution.
#include <ok/fs/ext4/mount.hpp>
template class ok::fs::Ext4OwnedTransaction<2, 8>;
template class ok::fs::Ext4MountedMetadata<2>;
struct CommonCompileOperation final
{
    template <typename Opening> ok::Status operator()(const Opening &) const
    { return ok::Status::unsupported("compile-only common opening operation"); }
};
template ok::Status ok::fs::Ext4MountedMetadata<2>::common_mount_scope<CommonCompileOperation>(
    ok::fs::Ext4OwnedTransaction<2, 8>::Registry &,
    ok::fs::Ext4OwnedTransaction<2, 8>::Registry::Registration,
    ok::block::SessionBounds,
    ok::fs::Ext4OwnedTransaction<2, 8>::TypedOwnerWorkspace &, CommonCompileOperation);
