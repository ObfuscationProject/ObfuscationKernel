#include "ok/fs/page_cache.hpp"

namespace ok::fs
{

FilePageCache::~FilePageCache()
{
    // Best effort: pinned or dirty pages stay owned rather than freed under a user.
    static_cast<void>(clear());
}

Status FilePageCache::bind(memory::MemoryManager &memory, memory::ChargeAccount &pages)
{
    if (memory_ != nullptr)
        return Status::already_exists("page cache is already bound");
    if (!memory.objects().initialized() || memory.frames().page_size() != 4096)
        return Status::not_initialized("page cache requires boot-discovered frames and kernel objects");
    memory_ = &memory;
    pages_ = &pages;
    return Status::success();
}

usize FilePageCache::position(u64 index) const
{
    usize low = 0;
    usize high = count_;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (entries_[middle].index < index)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

memory::PageDescriptor *FilePageCache::state(u64 index) const
{
    const usize at = position(index);
    if (at == count_ || entries_[at].index != index)
        return nullptr;
    auto *descriptor = memory_->frames().descriptor(entries_[at].frame);
    return descriptor && descriptor->kind == static_cast<u8>(memory::PageKind::page_cache) ? descriptor : nullptr;
}

Result<memory::PhysicalFrame> FilePageCache::lookup(u64 index) const
{
    if (memory_ == nullptr)
        return Status::not_initialized("page cache is not bound");
    const usize at = position(index);
    if (at == count_ || entries_[at].index != index)
        return Status::not_found("file page is not cached");
    return entries_[at].frame;
}

Status FilePageCache::grow(memory::AllocationContext context)
{
    const usize capacity = capacity_ ? capacity_ * 2 : 8;
    if (capacity > ~usize{0} / sizeof(Entry))
        return Status::overflow("page cache index overflows");
    auto storage = memory_->objects().allocate(capacity * sizeof(Entry), objects_, context);
    if (!storage)
        return storage.status();
    auto *entries = static_cast<Entry *>(storage.value());
    for (usize i = 0; i < count_; ++i)
        entries[i] = entries_[i];
    if (entries_ != nullptr)
        if (auto status = memory_->objects().release(entries_, capacity_ * sizeof(Entry), objects_); !status.ok())
        {
            static_cast<void>(memory_->objects().release(entries, capacity * sizeof(Entry), objects_));
            return status;
        }
    entries_ = entries;
    capacity_ = capacity;
    return Status::success();
}

Result<memory::PhysicalFrame> FilePageCache::obtain(u64 index, memory::AllocationContext context)
{
    if (auto found = lookup(index); found || found.status().code() != StatusCode::not_found)
        return found;
    if (count_ == capacity_)
        if (auto status = grow(context); !status.ok())
            return status;
    auto &frames = memory_->frames();
    if (context != memory::AllocationContext::atomic && frames.free_frames() <= memory_->objects().reserve_frames())
        return Status::no_memory("frame reserve is held for atomic allocations");
    auto frame = frames.allocate(*pages_);
    if (!frame)
        return frame.status();
    auto *descriptor = frames.descriptor(frame.value());
    descriptor->kind = static_cast<u8>(memory::PageKind::page_cache);
    descriptor->references = static_cast<u32>(index); // Low bits, for diagnostics only.
    auto *bytes = reinterpret_cast<std::byte *>(frame.value().address);
    for (usize i = 0; i < frames.page_size(); ++i)
        bytes[i] = std::byte{};
    const usize at = position(index);
    for (usize i = count_; i > at; --i)
        entries_[i] = entries_[i - 1];
    entries_[at] = {index, frame.value()};
    ++count_;
    return frame;
}

Status FilePageCache::mark_uptodate(u64 index)
{
    auto *descriptor = memory_ ? state(index) : nullptr;
    if (!descriptor)
        return Status::not_found("file page is not cached");
    descriptor->flags |= uptodate_flag;
    return Status::success();
}

Status FilePageCache::mark_dirty(u64 index)
{
    auto *descriptor = memory_ ? state(index) : nullptr;
    if (!descriptor)
        return Status::not_found("file page is not cached");
    if (!(descriptor->flags & uptodate_flag))
        return Status::invalid_argument("only an uptodate page can become dirty");
    descriptor->flags |= dirty_flag;
    return Status::success();
}

Status FilePageCache::mark_clean(u64 index)
{
    auto *descriptor = memory_ ? state(index) : nullptr;
    if (!descriptor)
        return Status::not_found("file page is not cached");
    descriptor->flags &= static_cast<u8>(~dirty_flag);
    return Status::success();
}

Result<u64> FilePageCache::next_dirty(u64 from) const
{
    if (memory_ == nullptr)
        return Status::not_initialized("page cache is not bound");
    for (usize at = position(from); at < count_; ++at)
    {
        const auto *descriptor = memory_->frames().descriptor(entries_[at].frame);
        if (descriptor && (descriptor->flags & dirty_flag))
            return entries_[at].index;
    }
    return Status::not_found("no dirty file page at or after the offset");
}

Status FilePageCache::pin(u64 index)
{
    auto *descriptor = memory_ ? state(index) : nullptr;
    if (!descriptor)
        return Status::not_found("file page is not cached");
    if (descriptor->pins == 65535)
        return Status::overflow("file page pin count exhausted");
    ++descriptor->pins;
    return Status::success();
}

Status FilePageCache::unpin(u64 index)
{
    auto *descriptor = memory_ ? state(index) : nullptr;
    if (!descriptor)
        return Status::not_found("file page is not cached");
    if (!descriptor->pins)
        return Status::invalid_argument("file page is not pinned");
    --descriptor->pins;
    return Status::success();
}

Status FilePageCache::remove_at(usize at)
{
    if (auto status = memory_->frames().release(entries_[at].frame, *pages_); !status.ok())
        return status;
    for (usize i = at + 1; i < count_; ++i)
        entries_[i - 1] = entries_[i];
    --count_;
    return Status::success();
}

Result<usize> FilePageCache::evict_clean()
{
    if (memory_ == nullptr)
        return Status::not_initialized("page cache is not bound");
    usize evicted = 0;
    for (usize at = count_; at-- != 0;)
    {
        const auto *descriptor = memory_->frames().descriptor(entries_[at].frame);
        if (!descriptor || descriptor->pins || (descriptor->flags & dirty_flag))
            continue;
        if (auto status = remove_at(at); !status.ok())
            return status;
        ++evicted;
    }
    return evicted;
}

Status FilePageCache::truncate(u64 first)
{
    if (memory_ == nullptr)
        return Status::not_initialized("page cache is not bound");
    // Mapped or in-flight pages cannot vanish under their users.
    for (usize at = position(first); at < count_; ++at)
        if (const auto *descriptor = memory_->frames().descriptor(entries_[at].frame); !descriptor || descriptor->pins)
            return Status::busy("a truncated file page is pinned");
    while (count_ != 0 && entries_[count_ - 1].index >= first)
        if (auto status = remove_at(count_ - 1); !status.ok())
            return status;
    return Status::success();
}

Status FilePageCache::clear()
{
    if (memory_ == nullptr)
        return Status::success();
    for (usize at = 0; at < count_; ++at)
    {
        const auto *descriptor = memory_->frames().descriptor(entries_[at].frame);
        if (!descriptor || descriptor->pins || (descriptor->flags & dirty_flag))
            return Status::busy("a cached file page is pinned or dirty");
    }
    while (count_ != 0)
        if (auto status = remove_at(count_ - 1); !status.ok())
            return status;
    if (entries_ != nullptr)
    {
        if (auto status = memory_->objects().release(entries_, capacity_ * sizeof(Entry), objects_); !status.ok())
            return status;
        entries_ = nullptr;
        capacity_ = 0;
    }
    return Status::success();
}

PageCacheAccounting FilePageCache::accounting() const
{
    PageCacheAccounting result{.pages = count_, .index_capacity = capacity_};
    for (usize at = 0; memory_ != nullptr && at < count_; ++at)
        if (const auto *descriptor = memory_->frames().descriptor(entries_[at].frame))
        {
            result.uptodate += (descriptor->flags & uptodate_flag) != 0;
            result.dirty += (descriptor->flags & dirty_flag) != 0;
            result.pinned += descriptor->pins != 0;
        }
    return result;
}

} // namespace ok::fs
