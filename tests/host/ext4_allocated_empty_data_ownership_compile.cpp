#include <ok/fs/ext4_allocated_empty_data_ownership.hpp>

struct AllocatedEmptyOwnershipCompileReader
{
    [[nodiscard]] ok::Status read_block(ok::u32 number, std::span<ok::u8> output);
};
ok::Status allocated_empty_ownership_compile_probe(const ok::fs::Ext4RecoveryInfo &binding,
    ok::fs::Ext4ReadOnlyView<AllocatedEmptyOwnershipCompileReader> &view,
    const ok::fs::Ext4AllocatedEmptyDataPlan &plan, ok::fs::Ext4AllocatedEmptyDataOwnershipScratch &scratch,
    ok::fs::Ext4AllocatedEmptyDataOwnership &output)
{
    return ok::fs::Ext4AllocatedEmptyDataOwnershipValidator::prove(binding, view, plan, scratch, output);
}
