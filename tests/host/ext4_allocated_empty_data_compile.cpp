#include <ok/fs/ext4/extent.hpp>

ok::Status allocated_empty_compile_probe(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4AllocatedEmptyDataUpdate &update,
    const ok::fs::Ext4AllocatedEmptyDataSource &source,
    ok::fs::Ext4AllocatedEmptyDataScratch &scratch, ok::fs::Ext4AllocatedEmptyDataPlan &output)
{
    return ok::fs::Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding, update, source, scratch, output);
}
