#include "ok/fs/ext4/txn.hpp"

using Cache = ok::fs::MetadataBuffers<8, 1024, 4>;

extern "C" ok::Status compile_metadata_retirement(Cache &cache,
    std::span<const ok::u64> released)
{
    const auto retired = cache.retire_clean_blocks(released);
    return retired ? ok::Status{} : retired.status();
}
