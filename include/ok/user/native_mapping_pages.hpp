#pragma once

#include "ok/user/native_page_pool.hpp"
#include <utility>

namespace ok::user::native
{
// Metadata ownership only. User frames are retired by the native runtime after
// their last backend translation has gone away. Records live in one contiguous
// kernel-object run, followed by one retirement byte per record for the
// runtime's quarantine; exact frame tokens live in a second run. Both runs grow
// by moving, so a directory refuses insertion and growth once a view of it has
// been published to a backend. The user window bounds the record count.
class MappingPages final
{
  public:
    using Mapping = arch::execution::PageMapping;
    static constexpr usize capacity = arch::execution::user_window_pages;
    static constexpr usize page_size = NativePagePool::page_size;
    static constexpr usize record_bytes = sizeof(Mapping) + 1;
    static constexpr usize records_per_page = page_size / record_bytes;
    static constexpr usize frames_per_page = page_size / sizeof(memory::PhysicalFrame);
    static_assert(records_per_page && frames_per_page);

    MappingPages() = default;
    MappingPages(const MappingPages &) = delete;
    MappingPages &operator=(const MappingPages &) = delete;
    MappingPages(MappingPages &&other) noexcept { swap(other); }
    MappingPages &operator=(MappingPages &&other) noexcept
    {
        if (this != &other) swap(other);
        return *this;
    }
    ~MappingPages()
    {
        // A run the pool refuses to take back stays accounted and diagnosable.
        if (!clear() && pool_)
        {
            if (frames_) static_cast<void>(pool_->quarantine_object_run(reinterpret_cast<uptr>(frames_), frame_pages_));
            if (records_) static_cast<void>(pool_->quarantine_object_run(reinterpret_cast<uptr>(records_), record_pages_));
        }
    }

    [[nodiscard]] usize size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity; }
    [[nodiscard]] usize reserved() const { return reserved_; }
    [[nodiscard]] usize record_pages() const { return record_pages_; }
    [[nodiscard]] usize frame_pages() const { return frame_pages_; }
    [[nodiscard]] bool exact_frames() const { return exact_frames_; }
    [[nodiscard]] bool published() const { return published_; }
    [[nodiscard]] const memory::PhysicalFrame *frame(usize index) const
    {
        return exact_frames_ && index < size_ ? &frames_[index] : nullptr;
    }
    [[nodiscard]] const memory::PhysicalFrame *frame(const Mapping &mapping) const
    {
        return &mapping >= records_ && &mapping < records_ + size_ ? frame(static_cast<usize>(&mapping - records_))
                                                                   : nullptr;
    }

    Status bind(NativePagePool &pool)
    {
        if (pool_ && pool_ != &pool)
            return Status::invalid_argument("mapping metadata has a different page owner");
        pool_ = &pool;
        return Status::success();
    }
    Status reserve(NativePagePool &pool, usize records, bool exact_frames = false)
    {
        if (auto status = bind(pool); !status) return status;
        if (records > capacity)
            return Status::no_memory("native process page capacity exceeded");
        if (exact_frames && !exact_frames_ && size_)
            return Status::invalid_argument("unowned metadata cannot become native frame authority");
        const bool require_frames = exact_frames_ || exact_frames;
        usize target = reserved_;
        if (records > reserved_)
        {
            // Whole pages for each run; with exact tokens the frame run, whose
            // entries are larger, decides how many records the pages hold.
            const usize wanted = records > reserved_ * 2 ? (records < capacity ? records : capacity)
                                                         : (reserved_ * 2 < capacity ? reserved_ * 2 : capacity);
            auto fitting = [&](usize bytes) { return (wanted * bytes + page_size - 1) / page_size * page_size / bytes; };
            target = fitting(record_bytes);
            if (require_frames && fitting(sizeof(memory::PhysicalFrame)) < target)
                target = fitting(sizeof(memory::PhysicalFrame));
            if (target > capacity) target = capacity;
        }
        if (!target)
        {
            exact_frames_ = require_frames;
            return Status::success();
        }
        const bool move_records = target != reserved_;
        const bool move_frames = require_frames && (move_records || !frames_);
        if (!move_records && !move_frames)
        {
            exact_frames_ = require_frames;
            return Status::success();
        }
        if (published_)
            return Status::busy("published mapping metadata cannot grow");
        const usize record_pages = (target * record_bytes + page_size - 1) / page_size;
        const usize frame_pages = (target * sizeof(memory::PhysicalFrame) + page_size - 1) / page_size;
        Mapping *records_run = records_;
        memory::PhysicalFrame *frames_run = frames_;
        if (move_records)
        {
            auto run = pool.allocate_object_run(record_pages);
            if (!run) return run.status();
            records_run = reinterpret_cast<Mapping *>(run.value());
        }
        if (move_frames)
        {
            auto run = pool.allocate_object_run(frame_pages);
            if (!run)
            {
                if (move_records)
                    static_cast<void>(pool.release_object_run(reinterpret_cast<uptr>(records_run), record_pages));
                return run.status();
            }
            frames_run = reinterpret_cast<memory::PhysicalFrame *>(run.value());
        }
        for (usize index = 0; index < size_; ++index)
        {
            if (move_records)
            {
                records_run[index] = records_[index];
                reinterpret_cast<u8 *>(records_run + target)[index] = retirement(index);
            }
            if (move_frames && frames_) frames_run[index] = frames_[index];
        }
        const auto previous_records = std::exchange(records_, records_run);
        const auto previous_frames = move_frames ? std::exchange(frames_, frames_run) : nullptr;
        const auto previous_record_pages = std::exchange(record_pages_, record_pages);
        const auto previous_frame_pages = move_frames ? std::exchange(frame_pages_, frame_pages) : 0;
        reserved_ = target;
        exact_frames_ = require_frames;
        Status result{};
        if (move_records && previous_records)
            result = retire_run(reinterpret_cast<uptr>(previous_records), previous_record_pages);
        if (previous_frames)
            if (auto status = retire_run(reinterpret_cast<uptr>(previous_frames), previous_frame_pages); !status && result)
                result = status;
        return result;
    }
    Status push_back(Mapping mapping)
    {
        if (exact_frames_)
            return Status::invalid_argument("native mapping requires its exact frame token");
        return insert(mapping, {});
    }
    Status push_back(Mapping mapping, memory::PhysicalFrame frame)
    {
        if (!pool_ || !exact_frames_ || !pool_->valid(frame) || frame.address != mapping.physical_address)
            return Status::invalid_argument("native mapping frame token is foreign, stale or unbound");
        return insert(mapping, frame);
    }
    // Copy-on-write resolution is the one in-place edit of a published record:
    // it keeps the address and swaps the frame and/or access. The caller then
    // re-synchronizes the live translation with execution::remap.
    Status replace(usize index, Mapping mapping, memory::PhysicalFrame frame)
    {
        if (index >= size_ || !pool_ || !exact_frames_ || !pool_->valid(frame) ||
            frame.address != mapping.physical_address || mapping.virtual_address != records_[index].virtual_address)
            return Status::invalid_argument("native mapping replacement is foreign, stale or moved");
        records_[index] = mapping;
        frames_[index] = frame;
        return Status::success();
    }

    [[nodiscard]] Mapping &operator[](usize index) { return records_[index]; }
    [[nodiscard]] const Mapping &operator[](usize index) const { return records_[index]; }
    [[nodiscard]] Mapping *begin() { return records_; }
    [[nodiscard]] Mapping *end() { return records_ + size_; }
    [[nodiscard]] const Mapping *begin() const { return records_; }
    [[nodiscard]] const Mapping *end() const { return records_ + size_; }

    // Taking a view publishes the directory: until clear(), records keep their
    // addresses and only replace() edits them.
    [[nodiscard]] arch::execution::MappingView view() const
    {
        published_ = true;
        return {records_, size_};
    }
    // Per-record retirement state for the runtime's quarantine; zero for every
    // record a directory gains.
    [[nodiscard]] u8 &retirement(usize index) { return reinterpret_cast<u8 *>(records_ + reserved_)[index]; }
    [[nodiscard]] u8 retirement(usize index) const { return reinterpret_cast<const u8 *>(records_ + reserved_)[index]; }

    [[nodiscard]] static Result<MappingPages> clone(const MappingPages &source, NativePagePool &pool)
    {
        MappingPages result;
        if (auto status = result.reserve(pool, source.size(), source.exact_frames_); !status)
            return status;
        for (usize index = 0; index < source.size(); ++index)
            if (auto status = source.exact_frames_ ? result.push_back(source[index], *source.frame(index))
                                                   : result.push_back(source[index]); !status)
                return status;
        return result;
    }

    // Metadata release is explicit and reports a failed run retirement. A
    // failed release leaves that run in this owner for a later retry.
    Status clear()
    {
        if (frames_)
        {
            if (auto status = pool_->release_object_run(reinterpret_cast<uptr>(frames_), frame_pages_); !status)
                return status;
            frames_ = nullptr;
            frame_pages_ = 0;
        }
        if (records_)
        {
            if (auto status = pool_->release_object_run(reinterpret_cast<uptr>(records_), record_pages_); !status)
                return status;
            records_ = nullptr;
            record_pages_ = 0;
        }
        size_ = reserved_ = 0;
        pool_ = nullptr;
        exact_frames_ = published_ = false;
        return Status::success();
    }
    void swap(MappingPages &other) noexcept
    {
        using std::swap;
        swap(pool_, other.pool_);
        swap(records_, other.records_);
        swap(frames_, other.frames_);
        swap(size_, other.size_);
        swap(reserved_, other.reserved_);
        swap(record_pages_, other.record_pages_);
        swap(frame_pages_, other.frame_pages_);
        swap(exact_frames_, other.exact_frames_);
        swap(published_, other.published_);
    }

  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct MappingPagesFaultProbe;
#endif
    Status insert(Mapping mapping, memory::PhysicalFrame frame)
    {
        if (!pool_)
            return Status::not_initialized("mapping metadata has no page owner");
        if (published_)
            return Status::busy("published mapping metadata cannot gain records");
        // Ordered records let backends search by address and make memory-area
        // coalescing a bounded scan without a process-sized stack copy.
        usize low = 0, high = size_;
        while (low < high)
        {
            const usize middle = low + (high - low) / 2;
            if (records_[middle].virtual_address < mapping.virtual_address) low = middle + 1;
            else high = middle;
        }
        if (low < size_ && records_[low].virtual_address == mapping.virtual_address)
            return Status::already_exists("duplicate native mapping record");
        if (auto status = reserve(*pool_, size_ + 1); !status) return status;
        for (usize index = size_; index > low; --index)
        {
            records_[index] = records_[index - 1];
            retirement(index) = retirement(index - 1);
            if (exact_frames_) frames_[index] = frames_[index - 1];
        }
        records_[low] = mapping;
        retirement(low) = 0;
        if (exact_frames_) frames_[low] = frame;
        ++size_;
        return Status::success();
    }
    Status retire_run(uptr address, usize pages)
    {
        if (auto status = pool_->release_object_run(address, pages); !status)
        {
            static_cast<void>(pool_->quarantine_object_run(address, pages));
            return status;
        }
        return Status::success();
    }
    NativePagePool *pool_{};
    Mapping *records_{};
    memory::PhysicalFrame *frames_{};
    usize size_{}, reserved_{};
    usize record_pages_{}, frame_pages_{};
    bool exact_frames_{};
    mutable bool published_{};
};

// One bit field of a record's retirement byte.
template <typename Value, u8 Mask, u8 Shift> struct RetirementField
{
    u8 *byte;
    operator Value() const { return static_cast<Value>((*byte & Mask) >> Shift); }
    RetirementField &operator=(Value value)
    {
        *byte = static_cast<u8>((*byte & ~Mask) | ((static_cast<u8>(value) << Shift) & Mask));
        return *this;
    }
};
} // namespace ok::user::native
