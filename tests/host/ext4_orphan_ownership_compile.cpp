#include "ok/fs/ext4_orphan_ownership.hpp"

ok::Status compile_orphan_ownership(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4OrphanOwnershipSource &source,
    ok::fs::Ext4OrphanOwnershipScratch &scratch,
    ok::fs::Ext4OrphanOwnershipProof &proof)
{
    return ok::fs::Ext4OrphanOwnershipValidator::validate(binding, source, scratch, proof);
}
