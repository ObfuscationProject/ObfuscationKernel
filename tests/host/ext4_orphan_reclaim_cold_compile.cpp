#include "ok/fs/ext4_orphan_reclaim_cold.hpp"
ok::Status compile_reclaim_cold(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4OrphanReclaimColdRecords &records,
    const ok::fs::Ext4OrphanReclaimColdObserved &observed,
    ok::fs::Ext4OrphanReclaimColdScratch &scratch,
    ok::fs::Ext4OrphanReclaimColdResult &result)
{ return ok::fs::Ext4OrphanReclaimColdVerifier::verify(binding, records, observed, scratch, result); }
