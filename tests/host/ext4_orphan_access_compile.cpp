#include "ok/fs/ext4_orphan_access.hpp"

ok::Status compile_orphan_access(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4OrphanChainSource &source,
    const ok::fs::Ext4OrphanAccessKey &key,
    ok::fs::Ext4OrphanReadOnlySnapshot &snapshot)
{
    auto status = ok::fs::Ext4OrphanReadOnlyDecoder::capture(binding, source, key, snapshot);
    if (!status) return status;
    auto block = snapshot.physical_block(0);
    return block ? ok::Status::success() : block.status();
}
