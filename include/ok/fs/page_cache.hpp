#pragma once

#include "ok/memory/memory.hpp"

namespace ok::fs
{

struct PageCacheAccounting
{
    usize pages{};
    usize uptodate{};
    usize dirty{};
    usize pinned{};
    usize index_capacity{};
};

// Page-sized contents of one file, owned by its VFS node. Pages are frames of
// kind `page_cache` from the kernel allocator, charged to the cache's page
// account; the sorted page index lives in kernel objects charged to the same
// owner in bytes. Per-page state is the frame's PageDescriptor: `flags` holds
// uptodate/dirty, `pins` counts mappings and in-flight I/O.
//
// This is the skeleton later shared file mappings and storage write-back build
// on: it never reads or writes the backing store itself. A page is created
// zeroed and not uptodate; the filesystem fills it and marks it uptodate, a
// writer marks it dirty, write-back walks next_dirty() and marks it clean.
// Pinned pages are never evicted, truncated or released. Single serialized
// caller, like the allocators it uses.
class FilePageCache final : private NonCopyable
{
  public:
    FilePageCache() = default;
    ~FilePageCache();

    // Binds the allocators; the page account limits the cache's frames.
    Status bind(memory::MemoryManager &memory, memory::ChargeAccount &pages);
    [[nodiscard]] bool bound() const
    {
        return memory_ != nullptr;
    }

    Result<memory::PhysicalFrame> lookup(u64 index) const;
    // Existing page or a new zeroed one that is not uptodate.
    Result<memory::PhysicalFrame> obtain(u64 index,
                                         memory::AllocationContext context = memory::AllocationContext::kernel);
    Status mark_uptodate(u64 index);
    Status mark_dirty(u64 index);
    Status mark_clean(u64 index);
    [[nodiscard]] Result<u64> next_dirty(u64 from) const;
    Status pin(u64 index);
    Status unpin(u64 index);

    // Releases every unpinned clean page; returns how many were released.
    Result<usize> evict_clean();
    // Discards pages at or past `first` (file shrink); pinned pages make it busy.
    Status truncate(u64 first);
    // Node retirement: refuses while any page is pinned or dirty.
    Status clear();

    [[nodiscard]] PageCacheAccounting accounting() const;

  private:
    struct Entry
    {
        u64 index{};
        memory::PhysicalFrame frame{};
    };
    static constexpr u8 uptodate_flag = 1, dirty_flag = 2;

    [[nodiscard]] usize position(u64 index) const;
    [[nodiscard]] memory::PageDescriptor *state(u64 index) const;
    Status grow(memory::AllocationContext context);
    Status remove_at(usize position);

    memory::MemoryManager *memory_{};
    memory::ChargeAccount *pages_{};
    memory::ChargeAccount objects_{};
    Entry *entries_{};
    usize count_{};
    usize capacity_{};
};

} // namespace ok::fs
