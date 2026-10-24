#include <ok/fs/ext4_readonly.hpp>

struct FirstDataCompileReader
{
    ok::Status read_block(ok::u32, std::span<ok::u8>)
    { return ok::Status::unsupported("compile-only first-data reader"); }
};
template class ok::fs::Ext4ReadOnlyView<FirstDataCompileReader>;
