#include <ok/fs/ext4_zero_truncate_plan.hpp>
#include <ok/fs/ext4_zero_truncate_witness.hpp>
#include <ok/fs/ext4_zero_truncate_preview.hpp>
#include <ok/fs/ext4_zero_truncate_session_preview.hpp>
#include <ok/fs/ext4_zero_truncate_journal.hpp>

using namespace ok;
using namespace ok::fs;
Status compile_zero_truncate_plan(const Ext4RecoveryInfo &binding, const Ext4ZeroTruncateUpdate &update,
    const Ext4ZeroTruncateSource &source, Ext4ZeroTruncatePlan &scratch, Ext4ZeroTruncatePlan &output)
{ return Ext4ZeroTruncatePlanner::plan_off_stack(binding, update, source, scratch, output); }

struct ZeroTruncateCompileReader
{
    Status read_block(u32, std::span<u8>);
};
Status compile_zero_truncate_ownership(const Ext4RecoveryInfo &binding,
    Ext4ReadOnlyView<ZeroTruncateCompileReader> &view, const Ext4ZeroTruncateUpdate &update,
    Ext4ZeroTruncateOwnershipScratch &scratch, Ext4ZeroTruncateOwnership &output)
{ return Ext4ZeroTruncateOwnershipValidator::prove(binding, view, update, scratch, output); }

Status compile_zero_truncate_witness_encode(const Ext4RecoveryInfo &binding,
    const Ext4ZeroTruncatePlan &plan, const Ext4ZeroTruncateBlock &inode_bitmap, u32 sequence,
    Ext4ZeroTruncateWitnessScratch &scratch, Ext4ZeroTruncateBlock &descriptor)
{ return Ext4ZeroTruncateWitnessCodec::encode(binding, plan, inode_bitmap, sequence, scratch, descriptor); }

Status compile_zero_truncate_witness_decode(const Ext4RecoveryInfo &binding,
    const Ext4ZeroTruncateBlock &descriptor, std::span<const Ext4ZeroTruncateBlock, 4> after,
    const Ext4ZeroTruncateBlock &inode_bitmap, Ext4ZeroTruncateWitnessScratch &scratch,
    Ext4ZeroTruncatePlan &output, Ext4ZeroTruncateWitness &witness)
{ return Ext4ZeroTruncateWitnessCodec::decode(binding, descriptor, after, inode_bitmap, scratch, output, witness); }

Status compile_zero_truncate_journal_verify(const Ext4RecoveryInfo &binding,
    const Ext4ZeroTruncateJournalRecords &records, const Ext4ZeroTruncateBlock &inode_bitmap,
    std::span<const Ext4ZeroTruncateBlock> homes, Ext4ZeroTruncateJournalScratch &scratch,
    Ext4ZeroTruncateJournalResult &output)
{ return Ext4ZeroTruncateJournalVerifier::verify(binding, records, inode_bitmap, homes, scratch, output); }

Status compile_zero_truncate_preview(ZeroTruncateCompileReader &reader, const Ext4RecoveryInfo &binding,
    Ext4ZeroTruncatePreviewWorkspace<ZeroTruncateCompileReader> &workspace,
    Ext4ZeroTruncatePreview<ZeroTruncateCompileReader> &preview)
{ return preview.prepare(reader, binding, workspace); }

Status compile_zero_truncate_session_preview(block::BlockSessions<4>::Session &session,
    const Ext4RecoveryInfo &binding,
    Ext4ZeroTruncateSessionPreview<block::BlockSessions<4>::Session> &preview)
{ return preview.prepare(session, binding); }

Status compile_zero_truncate_journal_format(const Ext4RecoveryInfo &binding,
    const Ext4ZeroTruncatePlan &plan, const Ext4ZeroTruncateBlock &head, const Ext4ZeroTruncateBlock &inode_bitmap,
    Ext4ZeroTruncateJournalFormatScratch &scratch, Ext4ZeroTruncateJournalFormat &output)
{ return Ext4ZeroTruncateJournalFormatter::format(binding, plan, head, inode_bitmap, scratch, output); }
