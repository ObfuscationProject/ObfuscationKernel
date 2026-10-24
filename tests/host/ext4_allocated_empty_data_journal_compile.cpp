#include <ok/fs/ext4_allocated_empty_data_journal.hpp>

ok::Status allocated_empty_journal_format_probe(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4AllocatedEmptyDataPlan &plan, const ok::fs::Ext4FirstDataBlock &head,
    const ok::fs::Ext4FirstDataBlock &inode_bitmap, ok::fs::Ext4AllocatedEmptyDataJournalFormatScratch &scratch,
    ok::fs::Ext4AllocatedEmptyDataJournalFormat &output)
{
    return ok::fs::Ext4AllocatedEmptyDataJournalFormatter::format(binding, plan, head, inode_bitmap, scratch, output);
}
ok::Status allocated_empty_journal_verify_probe(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4AllocatedEmptyDataJournalRecords &records, const ok::fs::Ext4FirstDataBlock &inode_bitmap,
    std::span<const ok::fs::Ext4FirstDataBlock> actual, ok::fs::Ext4AllocatedEmptyDataJournalScratch &scratch,
    ok::fs::Ext4AllocatedEmptyDataJournalResult &output)
{
    return ok::fs::Ext4AllocatedEmptyDataJournalVerifier::verify(binding, records, inode_bitmap, actual, scratch, output);
}
