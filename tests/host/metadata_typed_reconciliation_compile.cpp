#include <ok/fs/metadata_buffers.hpp>

// Freestanding host compiler probe only; no production owner is attached.
namespace ok::fs
{
template <usize Capacity, usize Devices> class Ext4OwnedTransaction
{
  public:
    using Cache = MetadataBuffers<Capacity, 1024, Devices>;
    using Change = typename Cache::TypedCacheChange;
    Ext4OwnedTransaction() = default;
    ~Ext4OwnedTransaction() = default;
    typename Cache::TypedReconciliation plan{};
    [[nodiscard]] Status prepare(Cache &cache, std::span<const Change> changes)
    { return cache.prepare_typed_reconciliation(changes, plan); }
    [[nodiscard]] Status normalize(Cache &cache)
    { return cache.normalize_typed_reconciliation(plan); }
    [[nodiscard]] Status publish(Cache &cache)
    { return cache.publish_typed_reconciliation(plan); }
};
template class Ext4OwnedTransaction<8, 4>;
}
