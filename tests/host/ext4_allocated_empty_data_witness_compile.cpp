#include <ok/fs/ext4/extent.hpp>

ok::Status allocated_empty_witness_encode_probe(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4AllocatedEmptyDataPlan &plan, const ok::fs::Ext4FirstDataBlock &inode_bitmap,
    ok::u32 sequence, ok::fs::Ext4AllocatedEmptyDataWitnessScratch &scratch,
    ok::fs::Ext4FirstDataBlock &descriptor)
{
    return ok::fs::Ext4AllocatedEmptyDataWitnessCodec::encode(binding, plan, inode_bitmap,
        sequence, scratch, descriptor);
}
ok::Status allocated_empty_witness_decode_probe(const ok::fs::Ext4RecoveryInfo &binding,
    const ok::fs::Ext4FirstDataBlock &descriptor, std::span<const ok::fs::Ext4FirstDataBlock, 4> after,
    const ok::fs::Ext4FirstDataBlock &inode_bitmap, ok::fs::Ext4AllocatedEmptyDataWitnessScratch &scratch,
    ok::fs::Ext4AllocatedEmptyDataPlan &output, ok::fs::Ext4AllocatedEmptyDataWitness &witness)
{
    return ok::fs::Ext4AllocatedEmptyDataWitnessCodec::decode(binding, descriptor, after,
        inode_bitmap, scratch, output, witness);
}
