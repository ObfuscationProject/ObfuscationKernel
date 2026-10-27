#include <ok/fs/ext4/recovery.hpp>

ok::Status compile_namespace_cold(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4NamespaceReplayRecords &records,
    const ok::fs::Ext4NamespaceColdAncillary &ancillary,
    ok::fs::Ext4NamespaceReplayHomes homes, ok::fs::Ext4NamespaceColdScratch &scratch,
    ok::fs::Ext4NamespaceColdResult &result)
{
    return ok::fs::Ext4NamespaceColdReplayVerifier::verify(binding, records, ancillary, homes, scratch, result);
}
