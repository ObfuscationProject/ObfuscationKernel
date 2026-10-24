#include "ok/fs/ext4_orphan_reclaim_journal.hpp"

ok::Status compile_orphan_reclaim_journal(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4OrphanReclaimPlan &reclaim,
    const ok::fs::Ext4OrphanBlock &old_journal,
    ok::fs::Ext4OrphanReclaimJournalScratch &scratch,
    ok::fs::Ext4OrphanReclaimJournalPlan &output)
{
    return ok::fs::Ext4OrphanReclaimJournalPlanner::plan(
        binding, reclaim, old_journal, 1, 0, scratch, output);
}
