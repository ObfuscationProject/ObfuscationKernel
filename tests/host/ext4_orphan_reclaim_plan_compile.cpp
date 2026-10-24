#include "ok/fs/ext4_orphan_reclaim_plan.hpp"

ok::Status compile_orphan_reclaim_plan(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4OrphanOwnershipSource &source,
    ok::fs::Ext4OrphanOwnershipScratch &scratch,
    ok::fs::Ext4OrphanReclaimPlan &plan)
{
    return ok::fs::Ext4OrphanReclaimPlanner::plan(binding, source, scratch, 12, 0, plan);
}
