#include <ok/fs/ext4/dir.hpp>

ok::Status compile_namespace_journal(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4NamespaceUpdate &update, const ok::fs::Ext4NamespaceSource &source,
    const ok::fs::Ext4NamespaceBlock &head, ok::fs::Ext4NamespaceJournalScratch &scratch,
    ok::fs::Ext4NamespaceJournalPlan &output)
{ return ok::fs::Ext4NamespaceJournalPlanner::plan(binding, update, source, head, scratch, output); }
