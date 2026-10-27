// Normal-access freestanding instantiation of distinct cold startup semantics.
#include <ok/fs/ext4/mount.hpp>
template class ok::fs::Ext4OwnedTransaction<2, 8>;
template class ok::fs::Ext4MountedMetadata<2>;
// Explicit instantiation also compiles the synchronous local opening scope;
// access control on an explicit-instantiation declaration is standard C++.
struct ColdCompileOperation {
    template <typename Opening> ok::Status operator()(const Opening &) const
    { return ok::Status::unsupported("compile-only original cold operation"); }
};
template ok::Status ok::fs::Ext4MountedMetadata<2>::typed_cold_mount_scope<ColdCompileOperation>(
    ok::fs::Ext4OwnedTransaction<2, 8>::Registry &,
    ok::fs::Ext4OwnedTransaction<2, 8>::Registry::Registration,
    ok::block::SessionBounds,
    ok::fs::Ext4OwnedTransaction<2, 8>::TypedOwnerWorkspace &, ColdCompileOperation);
