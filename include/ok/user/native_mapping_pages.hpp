#pragma once

#include "ok/user/native_page_pool.hpp"
#include <array>
#include <utility>

namespace ok::user::native
{
// Metadata ownership only. User frames are retired by the native runtime after
// their last backend translation has gone away. The directory is bounded, but
// every PageMapping record lives in a supervisor page leased from the pool.
class MappingPages final
{
  public:
    using Mapping = arch::execution::PageMapping;
    static constexpr usize capacity = arch::execution::max_native_mappings;
    static constexpr usize records_per_block = NativePagePool::page_size / sizeof(Mapping);
    static constexpr usize max_blocks = (capacity + records_per_block - 1) / records_per_block;
    static constexpr usize frames_per_block = NativePagePool::page_size / sizeof(memory::PhysicalFrame);
    static constexpr usize max_frame_blocks = (capacity + frames_per_block - 1) / frames_per_block;
    static_assert(records_per_block && max_blocks == arch::execution::MappingView::max_chunks);

    MappingPages() = default;
    MappingPages(const MappingPages &) = delete;
    MappingPages &operator=(const MappingPages &) = delete;
    MappingPages(MappingPages &&other) noexcept { swap(other); }
    MappingPages &operator=(MappingPages &&other) noexcept
    {
        if (this != &other) swap(other);
        return *this;
    }
    ~MappingPages() { static_cast<void>(clear()); }

    [[nodiscard]] usize size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity; }
    [[nodiscard]] usize block_count() const { return blocks_; }
    [[nodiscard]] usize frame_block_count() const { return frame_blocks_; }
    [[nodiscard]] bool exact_frames() const { return exact_frames_; }
    [[nodiscard]] const memory::PhysicalFrame *frame(usize index) const
    {
        return exact_frames_ && index < size_ ? frame_slot(index) : nullptr;
    }
    [[nodiscard]] const memory::PhysicalFrame *frame(const Mapping &mapping) const
    {
        for (usize index = 0; index < size_; ++index)
            if (&(*this)[index] == &mapping) return frame(index);
        return nullptr;
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
        const usize required = (records + records_per_block - 1) / records_per_block;
        const usize original = blocks_;
        const usize original_frames = frame_blocks_;
        const bool require_frames = exact_frames_ || exact_frames;
        if (exact_frames && !exact_frames_ && size_)
            return Status::invalid_argument("unowned metadata cannot become native frame authority");
        auto unwind = [&]() -> Status {
            while (frame_blocks_ > original_frames)
            {
                if (auto status = pool.release_kernel_page(frame_leases_[frame_blocks_ - 1]); !status) return status;
                --frame_blocks_;
            }
            while (blocks_ > original)
            {
                if (auto status = pool.release_kernel_page(leases_[blocks_ - 1]); !status) return status;
                --blocks_;
            }
            return {};
        };
        while (blocks_ < required)
        {
            auto lease = pool.allocate_kernel_page();
            if (!lease)
            {
                if (auto status = unwind(); !status) return status;
                return lease.status();
            }
            leases_[blocks_++] = std::move(lease.value());
        }
        const usize required_frames = require_frames ? (records + frames_per_block - 1) / frames_per_block : 0;
        while (frame_blocks_ < required_frames)
        {
            auto lease = pool.allocate_kernel_page();
            if (!lease)
            {
                if (auto status = unwind(); !status) return status;
                return lease.status();
            }
            frame_leases_[frame_blocks_++] = std::move(lease.value());
        }
        exact_frames_ = require_frames;
        return Status::success();
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
  private:
    Status insert(Mapping mapping, memory::PhysicalFrame frame)
    {
        if (!pool_)
            return Status::not_initialized("mapping metadata has no page owner");
        usize position = 0;
        while (position < size_ && (*this)[position].virtual_address < mapping.virtual_address)
            ++position;
        if (position < size_ && (*this)[position].virtual_address == mapping.virtual_address)
            return Status::already_exists("duplicate native mapping record");
        if (auto status = reserve(*pool_, size_ + 1); !status) return status;
        // Ordered records make memory-area coalescing a bounded scan without a
        // process-sized stack copy. Existing published blocks are never edited.
        for (usize slot_index = size_; slot_index > position; --slot_index)
        {
            *slot(slot_index) = (*this)[slot_index - 1];
            if (exact_frames_) *frame_slot(slot_index) = *frame_slot(slot_index - 1);
        }
        *slot(position) = mapping;
        if (exact_frames_) *frame_slot(position) = frame;
        ++size_;
        return Status::success();
    }
  public:
    [[nodiscard]] Mapping &operator[](usize index) { return *slot(index); }
    [[nodiscard]] const Mapping &operator[](usize index) const { return *slot(index); }

    struct Iterator
    {
        MappingPages *owner{};
        usize index{};
        [[nodiscard]] Mapping &operator*() const { return (*owner)[index]; }
        Iterator &operator++() { ++index; return *this; }
        [[nodiscard]] bool operator!=(Iterator other) const { return index != other.index; }
    };
    struct ConstIterator
    {
        const MappingPages *owner{};
        usize index{};
        [[nodiscard]] const Mapping &operator*() const { return (*owner)[index]; }
        ConstIterator &operator++() { ++index; return *this; }
        [[nodiscard]] bool operator!=(ConstIterator other) const { return index != other.index; }
    };
    [[nodiscard]] Iterator begin() { return {this, 0}; }
    [[nodiscard]] Iterator end() { return {this, size_}; }
    [[nodiscard]] ConstIterator begin() const { return {this, 0}; }
    [[nodiscard]] ConstIterator end() const { return {this, size_}; }

    [[nodiscard]] arch::execution::MappingView view() const
    {
        arch::execution::MappingView result;
        result.count = size_;
        for (usize block = 0; block < blocks_; ++block)
        {
            const usize first = block * records_per_block;
            const usize count = size_ > first ?
                (size_ - first < records_per_block ? size_ - first : records_per_block) : 0;
            result.chunks[block] = {reinterpret_cast<const Mapping *>(leases_[block].bytes.data()), count};
        }
        return result;
    }

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

    // Metadata release is explicit and reports a failed lease retirement. A
    // failed release leaves that lease in this owner for a later retry.
    Status clear()
    {
        size_ = 0;
        while (frame_blocks_)
        {
            if (auto status = pool_->release_kernel_page(frame_leases_[frame_blocks_ - 1]); !status)
                return status;
            --frame_blocks_;
        }
        while (blocks_)
        {
            if (auto status = pool_->release_kernel_page(leases_[blocks_ - 1]); !status)
                return status;
            --blocks_;
        }
        pool_ = nullptr;
        exact_frames_ = false;
        return Status::success();
    }
    void swap(MappingPages &other) noexcept
    {
        using std::swap;
        swap(pool_, other.pool_);
        swap(size_, other.size_);
        swap(blocks_, other.blocks_);
        swap(frame_blocks_, other.frame_blocks_);
        swap(exact_frames_, other.exact_frames_);
        for (usize i = 0; i < max_blocks; ++i)
            swap(leases_[i], other.leases_[i]);
        for (usize i = 0; i < max_frame_blocks; ++i)
            swap(frame_leases_[i], other.frame_leases_[i]);
    }

  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct MappingPagesFaultProbe;
#endif
    [[nodiscard]] Mapping *slot(usize index) const
    {
        const usize block = index / records_per_block;
        return reinterpret_cast<Mapping *>(leases_[block].bytes.data()) + index % records_per_block;
    }
    [[nodiscard]] memory::PhysicalFrame *frame_slot(usize index) const
    {
        return reinterpret_cast<memory::PhysicalFrame *>(frame_leases_[index / frames_per_block].bytes.data()) +
            index % frames_per_block;
    }
    NativePagePool *pool_{};
    std::array<NativePagePool::Lease, max_blocks> leases_{};
    std::array<NativePagePool::Lease, max_frame_blocks> frame_leases_{};
    usize size_{}, blocks_{};
    usize frame_blocks_{};
    bool exact_frames_{};
};
} // namespace ok::user::native
