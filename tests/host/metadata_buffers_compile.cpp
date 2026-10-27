#include <ok/fs/ext4/txn.hpp>
using Cache = ok::fs::MetadataBuffers<2,1024,2>;
ok::Status qualify_metadata_buffers(Cache &cache, Cache::Registry &registry,
                                     Cache::Registry::Registration key,
                                     Cache::Pin &pin, Cache::Transaction &transaction,
                                     std::span<std::byte> bytes)
{
    if (auto status=cache.open(registry,key,{0,64},ok::block::SessionAccess::writable);!status)return status;
    if (auto status=cache.pin(1,pin);!status)return status;
    std::array<Cache::Pin*,1> pins{&pin};
    if (auto status=cache.begin(pins,transaction);!status)return status;
    if (auto status=transaction.edit(1,0,bytes);!status)return status;
    if (auto status=transaction.read(1,0,bytes);!status)return status;
    if (auto status=transaction.discard();!status)return status;
    if (auto status=pin.release();!status)return status;
    return cache.close();
}
