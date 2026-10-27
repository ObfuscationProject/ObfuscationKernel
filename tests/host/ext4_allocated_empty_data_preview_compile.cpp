#include <ok/fs/ext4/extent.hpp>
struct AllocatedEmptyPreviewCompileReader
{
    [[nodiscard]] ok::Status read_block(ok::u32 number, std::span<ok::u8> output);
};
template class ok::fs::Ext4AllocatedEmptyDataPreview<AllocatedEmptyPreviewCompileReader>;
