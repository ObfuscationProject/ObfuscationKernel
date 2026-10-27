#include <ok/fs/ext4/recovery.hpp>

ok::Result<ok::fs::Ext4ReplayShape> compile_shape(const ok::fs::Ext4RecoveryInfo &binding,
    ok::fs::Ext4ReplayDirectoryDomain directories, const ok::fs::jbd2::ReplayPlan &plan)
{ return ok::fs::Ext4ReplayShapePlanner::classify(binding, directories, plan); }
ok::Result<ok::fs::Ext4ReplayFamily> compile_select(ok::fs::Ext4ReplayShape shape,
    std::span<const ok::fs::Ext4ReplaySemanticDecision> decisions)
{ return ok::fs::Ext4ReplayShapePlanner::select(shape, decisions); }
