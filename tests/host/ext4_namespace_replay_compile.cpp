#include <ok/fs/ext4/recovery.hpp>

ok::Status compile_namespace_replay(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4NamespaceUpdate &update,
    const ok::fs::Ext4NamespaceSource &old_source,
    const ok::fs::Ext4NamespaceBlock &old_head,
    const ok::fs::Ext4NamespaceReplayRecords &records,
    ok::fs::Ext4NamespaceReplayHomes homes,
    ok::fs::Ext4NamespaceReplayScratch &scratch,
    ok::fs::Ext4NamespaceReplayResult &result)
{
    return ok::fs::Ext4NamespaceReplayVerifier::verify(binding, update, old_source,
        old_head, records, homes, scratch, result);
}
