#pragma once
#include "ok/arch/execution.hpp"
#include "ok/memory/memory.hpp"
#include <utility>

namespace ok::user::native
{
// Canonical ownership for the serialized boot-CPU runtime. PageMapping remains
// an architecture interface, not an allocation handle. Only this registry may
// return retained owner/generation tokens to the allocator. Activation requires
// already established supervisor access to every supplied frame address.
class NativePagePool final : private NonCopyable
{
  public:
    static constexpr usize capacity = 1024;
    static constexpr usize page_size = arch::execution::page_size;
    enum class Source : u8 { inactive, linker_reserved, qualified_firmware };
    class Lease final
    {
      public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept { *this = std::move(other); }
        Lease &operator=(Lease &&other) noexcept
        {
            if (this != &other)
            {
                // A discarded active token must remain discoverable in the
                // pool, even if a caller accidentally overwrites its lease.
                if (pool_)
                    static_cast<void>(pool_->quarantine_kernel_page(*this));
                bytes = other.bytes;
                pool_ = other.pool_;
                frame_ = other.frame_;
                other.bytes = {};
                other.pool_ = nullptr;
                other.frame_ = {};
            }
            return *this;
        }
        constexpr ~Lease()
        {
            if (pool_)
                static_cast<void>(pool_->quarantine_kernel_page(*this));
        }
        std::span<std::byte> bytes{};

      private:
        friend class NativePagePool;
        Lease(NativePagePool *pool, memory::PhysicalFrame frame)
            : bytes(reinterpret_cast<std::byte *>(frame.address), page_size), pool_(pool), frame_(frame) {}
        NativePagePool *pool_{};
        memory::PhysicalFrame frame_{};
    };
    Status activate(memory::FrameAllocator &frames, Source source, usize kernel_reserve_pages = 0)
    {
        if (frames_)
            return Status::already_exists("native page pool already active");
        if ((source != Source::linker_reserved && source != Source::qualified_firmware) ||
            frames.page_size() != page_size || !frames.total_frames() || frames.total_frames() > capacity ||
            frames.free_frames() != frames.total_frames() || kernel_reserve_pages > frames.total_frames())
            return Status::invalid_argument("native frame pool contract mismatch");
        frames_ = &frames;
        source_ = source;
        kernel_reserve_ = kernel_reserve_pages;
        return Status::success();
    }
    Result<uptr> allocate()
    {
        auto acquired = acquire(Purpose::user);
        return acquired ? Result<uptr>(acquired.value().address) : Result<uptr>(acquired.status());
    }
    Result<memory::PhysicalFrame> allocate_frame() { return acquire(Purpose::user); }
    Result<Lease> allocate_kernel_page()
    {
        auto acquired = acquire(Purpose::object);
        if (!acquired)
            return acquired.status();
        const auto &frame = acquired.value();
        if (!frame.address || frame.address % page_size || frame.address > ~uptr{0} - (page_size - 1))
        {
            if (auto status = release_index(frame.index); !status)
                return status;
            return Status::fault("kernel page backing address is not a complete aligned page");
        }
        // Preserve the established supervisor address verbatim. In particular,
        // MIPS linker backing is a KSEG alias, not a low physical bus address.
        return Lease{this, frame};
    }
    Status release_kernel_page(Lease &lease)
    {
        const auto &frame = lease.frame_;
        if (lease.pool_ != this || frame.owner != frames_ || frame.index >= capacity || !frame.generation ||
            purpose_[frame.index] != Purpose::object || !same_frame(handles_[frame.index], frame) ||
            lease.bytes.data() != reinterpret_cast<std::byte *>(frame.address) || lease.bytes.size() != page_size)
            return Status::invalid_argument("kernel page lease is foreign, modified or stale");
        if (auto status = release_index(frame.index); !status)
            return status;
        lease.bytes = {};
        lease.pool_ = nullptr;
        lease.frame_ = {};
        return Status::success();
    }
    // This is a last-resort ownership transfer, not a retry of a failed
    // release. The canonical frame handle remains in this pool and cannot be
    // reallocated; diagnostics can identify the exact retained generation.
    Status quarantine_kernel_page(Lease &lease)
    {
        const auto &frame = lease.frame_;
        if (lease.pool_ != this || frame.owner != frames_ || frame.index >= capacity ||
            !frame.generation || purpose_[frame.index] != Purpose::object ||
            !same_frame(handles_[frame.index], frame))
            return Status::invalid_argument("kernel page lease cannot be quarantined");
        quarantined_objects_[frame.index] = true;
        lease.bytes = {};
        lease.pool_ = nullptr;
        lease.frame_ = {};
        return Status::success();
    }
    [[nodiscard]] usize quarantined_kernel_page_count() const
    {
        usize count = 0;
        for (bool retained : quarantined_objects_) count += retained;
        return count;
    }
    [[nodiscard]] const memory::PhysicalFrame *quarantined_kernel_page(usize index) const
    {
        return index < capacity && quarantined_objects_[index] ? &handles_[index] : nullptr;
    }
    [[nodiscard]] bool owns(uptr address) const { return find_user(address) != capacity; }
    [[nodiscard]] bool valid(const memory::PhysicalFrame &frame) const { return live_user(frame); }
    // Independent table/waiter holds are visible only for an exact canonical
    // allocator generation. An address may already name a different owner.
    [[nodiscard]] Result<usize> pin_count(const memory::PhysicalFrame &frame) const
    {
        if (!live_user(frame))
            return Status::invalid_argument("native frame pin query is foreign or stale");
        return usize{pins_[frame.index]};
    }
    Status mark_shared(const memory::PhysicalFrame &frame)
    {
        if (!live_user(frame) || references_[frame.index] != 1 || pending_[frame.index])
            return Status::invalid_argument("shared frame classification requires one live owner");
        shared_[frame.index] = true;
        return Status::success();
    }
    Status retain_shared(const memory::PhysicalFrame &frame)
    {
        if (!live_user(frame) || !shared_[frame.index] || !references_[frame.index] || pending_[frame.index])
            return Status::invalid_argument("shared frame is foreign, stale or pending release");
        if (references_[frame.index] == 65535)
            return Status::overflow("shared frame reference count exhausted");
        ++references_[frame.index];
        return Status::success();
    }
    Status release_reference(const memory::PhysicalFrame &frame)
    {
        if (!live_user(frame) || !references_[frame.index])
            return Status::invalid_argument("native user frame reference is foreign or stale");
#if defined(OK_ENABLE_TEST_POINTS)
        if (refuse_all_reference_releases_ || (refuse_reference_releases_ && same_frame(refused_reference_, frame)))
        {
            if (refuse_reference_releases_) --refuse_reference_releases_;
            return Status::fault("injected native pool reference release refusal");
        }
#endif
        const auto index = frame.index;
        if (references_[index] > 1)
        {
            --references_[index];
            return Status::success();
        }
        // A retired table can still borrow this page after the last process
        // reference is gone. The pin is independent of the process count.
        if (pins_[index])
        {
            references_[index] = 0;
            pending_[index] = true;
            return Status::success();
        }
        // Keep the last reference intact if the allocator rejects its
        // canonical handle, so the failed release remains diagnosable.
        return release_index(index);
    }
    Status release(uptr address)
    {
        const auto index = find_user(address);
        if (index == capacity)
            return Status::not_found("native user frame has no retained owner");
        return release_reference(handles_[index]);
    }
    Status pin(uptr address)
    {
        const auto index = find_user(address);
        return index == capacity ? Status::fault("invalid native frame quarantine pin") : pin(handles_[index]);
    }
    // A waiter retains the exact allocator generation, never a mutable VM
    // record or an address that may later name a MAP_FIXED replacement.
    Status pin(const memory::PhysicalFrame &frame)
    {
        if (!live_user(frame) || pins_[frame.index] == 65535 || !references_[frame.index] || pending_[frame.index])
            return Status::fault("invalid native frame quarantine pin");
        ++pins_[frame.index];
        return Status::success();
    }
    Status unpin(uptr address)
    {
        const auto index = find_user(address);
        return index == capacity ? Status::fault("invalid native frame quarantine release") : unpin(handles_[index]);
    }
    Status unpin(const memory::PhysicalFrame &frame)
    {
        if (!live_user(frame) || !pins_[frame.index])
            return Status::fault("invalid native frame quarantine release");
#if defined(OK_ENABLE_TEST_POINTS)
        if (refuse_unpins_ && same_frame(refused_unpin_, frame))
        {
            --refuse_unpins_;
            return Status::fault("injected native pool unpin refusal");
        }
#endif
        if (pins_[frame.index] == 1 && pending_[frame.index] && !references_[frame.index])
            return release_index(frame.index);
        --pins_[frame.index];
        return Status::success();
    }
    [[nodiscard]] Source source() const { return source_; }
    // used_count is all registry-owned pages; pins and pending are user subsets.
    [[nodiscard]] usize used_count() const { return user_count_ + object_count_; }
    [[nodiscard]] usize user_count() const { return user_count_; }
    [[nodiscard]] usize object_count() const { return object_count_; }
    [[nodiscard]] usize total_count() const { return frames_ ? frames_->total_frames() : 0; }
    [[nodiscard]] usize free_count() const { return frames_ ? frames_->free_frames() : 0; }
    [[nodiscard]] usize kernel_reserve_count() const { return kernel_reserve_; }
    [[nodiscard]] usize user_capacity_count() const { return total_count() - kernel_reserve_; }
    [[nodiscard]] usize pinned_count() const
    {
        usize count = 0;
        for (auto pins : pins_) count += pins != 0;
        return count;
    }
    [[nodiscard]] usize pending_count() const
    {
        usize count = 0;
        for (auto pending : pending_) count += pending;
        return count;
    }
    [[nodiscard]] usize reference_count(uptr address) const
    {
        const auto index = find_user(address);
        return index == capacity ? 0 : references_[index];
    }
    [[nodiscard]] bool is_shared(uptr address) const
    {
        const auto index = find_user(address);
        return index != capacity && shared_[index];
    }
    [[nodiscard]] usize peak_count() const { return peak_; }
    [[nodiscard]] u64 allocations() const { return allocations_; }
    [[nodiscard]] u64 releases() const { return releases_; }
    [[nodiscard]] const memory::PhysicalFrame *handle(uptr address) const
    {
        const auto index = find_user(address);
        return index == capacity ? nullptr : &handles_[index];
    }
  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct NativePagePoolFaultProbe;
#endif
    enum class Purpose : u8 { free, user, object };
    [[nodiscard]] static bool same_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
    {
        return left.owner == right.owner && left.index == right.index && left.generation == right.generation &&
               left.address == right.address;
    }
    [[nodiscard]] bool live_user(const memory::PhysicalFrame &frame) const
    {
        return frames_ && frame.owner == frames_ && frame.index < capacity && frame.generation &&
               purpose_[frame.index] == Purpose::user && same_frame(handles_[frame.index], frame);
    }
    Result<memory::PhysicalFrame> acquire(Purpose purpose)
    {
        if (!frames_)
            return Status::not_initialized("native page pool inactive");
        if ((purpose == Purpose::user && user_count_ >= user_capacity_count()) ||
            (purpose == Purpose::object && object_count_ >= kernel_reserve_))
            return Status::no_memory("native page purpose quota exhausted");
        auto frame = frames_->allocate();
        if (!frame)
            return frame.status();
        const auto index = frame.value().index;
        if (index >= handles_.size() || handles_[index].owner || purpose_[index] != Purpose::free)
        {
            static_cast<void>(frames_->release(frame.value()));
            return Status::fault("native frame registry collision");
        }
        handles_[index] = frame.value();
        purpose_[index] = purpose;
        if (purpose == Purpose::user)
        {
            references_[index] = 1;
            shared_[index] = false;
            ++user_count_;
        }
        else ++object_count_;
        ++allocations_;
        const auto used = used_count();
        if (used > peak_) peak_ = used;
        return frame;
    }
    [[nodiscard]] usize find_user(uptr address) const
    {
        for (usize i = 0; i < handles_.size(); ++i)
            if (purpose_[i] == Purpose::user && handles_[i].owner && handles_[i].address == address)
                return i;
        return capacity;
    }
    Status release_index(usize index)
    {
        if (auto status = frames_->release(handles_[index]); !status)
            return status;
        if (purpose_[index] == Purpose::user) --user_count_;
        else --object_count_;
        handles_[index] = {};
        purpose_[index] = Purpose::free;
        quarantined_objects_[index] = false;
        pins_[index] = 0;
        pending_[index] = false;
        references_[index] = 0;
        shared_[index] = false;
        ++releases_;
        return Status::success();
    }
    memory::FrameAllocator *frames_{};
    Source source_{Source::inactive};
    std::array<memory::PhysicalFrame, capacity> handles_{};
    std::array<Purpose, capacity> purpose_{};
    std::array<bool, capacity> quarantined_objects_{};
    std::array<u16, capacity> pins_{};
    std::array<bool, capacity> pending_{};
    std::array<u16, capacity> references_{};
    std::array<bool, capacity> shared_{};
#if defined(OK_ENABLE_TEST_POINTS)
    memory::PhysicalFrame refused_reference_{};
    usize refuse_reference_releases_{};
    bool refuse_all_reference_releases_{};
    memory::PhysicalFrame refused_unpin_{};
    usize refuse_unpins_{};
#endif
    usize kernel_reserve_{}, user_count_{}, object_count_{}, peak_{};
    u64 allocations_{}, releases_{};
};
} // namespace ok::user::native
