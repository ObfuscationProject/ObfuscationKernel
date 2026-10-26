#pragma once
#include "ok/arch/execution.hpp"
#include "ok/memory/memory.hpp"
#include <optional>
#include <utility>

namespace ok::user::native
{
// Canonical ownership for the serialized boot-CPU runtime. PageMapping remains
// an architecture interface, not an allocation handle. Only this registry may
// return retained owner/generation tokens to the allocator. Activation requires
// already established supervisor access to every supplied frame address.
// Per-frame state lives in the allocator's PageDescriptor of each owned frame,
// so the registry grows with the allocator instead of a fixed table. The
// allocator may be shared: frames of other kinds are never touched.
class NativePagePool final : private NonCopyable
{
  public:
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
    // Exclusive form: the pool owns every frame of a dedicated allocator; user
    // pages may use all frames outside the kernel-object reserve.
    Status activate(memory::FrameAllocator &frames, Source source, usize kernel_reserve_pages = 0)
    {
        if (frames.free_frames() != frames.total_frames())
            return Status::invalid_argument("native frame pool contract mismatch");
        return activate(frames, source, kernel_reserve_pages,
                        kernel_reserve_pages > frames.total_frames() ? 0 : frames.total_frames() - kernel_reserve_pages);
    }
    // Shared form: explicit object reserve and user-page quota over an
    // allocator other owners also draw from.
    Status activate(memory::FrameAllocator &frames, Source source, usize kernel_reserve_pages, usize user_limit_pages)
    {
        if (frames_)
            return Status::already_exists("native page pool already active");
        if ((source != Source::linker_reserved && source != Source::qualified_firmware) ||
            frames.page_size() != page_size || !frames.total_frames() || kernel_reserve_pages > frames.total_frames() ||
            user_limit_pages > frames.total_frames() - kernel_reserve_pages)
            return Status::invalid_argument("native frame pool contract mismatch");
        frames_ = &frames;
        source_ = source;
        kernel_reserve_ = kernel_reserve_pages;
        user_limit_ = user_limit_pages;
        return Status::success();
    }
    Result<uptr> allocate()
    {
        auto acquired = acquire(memory::PageKind::native_user);
        return acquired ? Result<uptr>(acquired.value().address) : Result<uptr>(acquired.status());
    }
    Result<memory::PhysicalFrame> allocate_frame() { return acquire(memory::PageKind::native_user); }
    Result<Lease> allocate_kernel_page()
    {
        auto acquired = acquire(memory::PageKind::native_object);
        if (!acquired)
            return acquired.status();
        const auto &frame = acquired.value();
        if (!frame.address || frame.address % page_size || frame.address > ~uptr{0} - (page_size - 1))
        {
            if (auto status = release_frame(frame); !status)
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
        if (lease.pool_ != this || !live(frame, memory::PageKind::native_object) ||
            lease.bytes.data() != reinterpret_cast<std::byte *>(frame.address) || lease.bytes.size() != page_size)
            return Status::invalid_argument("kernel page lease is foreign, modified or stale");
        if (auto status = release_frame(frame); !status)
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
        auto *descriptor = lease.pool_ == this ? state(frame, memory::PageKind::native_object) : nullptr;
        if (!descriptor)
            return Status::invalid_argument("kernel page lease cannot be quarantined");
        if (!(descriptor->flags & quarantined_flag))
            ++quarantined_count_;
        descriptor->flags |= quarantined_flag;
        lease.bytes = {};
        lease.pool_ = nullptr;
        lease.frame_ = {};
        return Status::success();
    }
    [[nodiscard]] usize quarantined_kernel_page_count() const { return quarantined_count_; }
    // Allocator index of a quarantined kernel page.
    [[nodiscard]] std::optional<memory::PhysicalFrame> quarantined_kernel_page(usize index) const
    {
        if (!frames_)
            return std::nullopt;
        auto frame = frames_->handle_at(index);
        if (!frame)
            return std::nullopt;
        const auto *descriptor = state(frame.value(), memory::PageKind::native_object);
        return descriptor && (descriptor->flags & quarantined_flag) ? std::optional{frame.value()} : std::nullopt;
    }
    [[nodiscard]] bool owns(uptr address) const { return find_user(address).has_value(); }
    [[nodiscard]] bool valid(const memory::PhysicalFrame &frame) const { return live(frame, memory::PageKind::native_user); }
    // Independent table/waiter holds are visible only for an exact canonical
    // allocator generation. An address may already name a different owner.
    [[nodiscard]] Result<usize> pin_count(const memory::PhysicalFrame &frame) const
    {
        const auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor)
            return Status::invalid_argument("native frame pin query is foreign or stale");
        return usize{descriptor->pins};
    }
    Status mark_shared(const memory::PhysicalFrame &frame)
    {
        auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor || descriptor->references != 1 || (descriptor->flags & pending_flag))
            return Status::invalid_argument("shared frame classification requires one live owner");
        descriptor->flags |= shared_flag;
        return Status::success();
    }
    Status retain_shared(const memory::PhysicalFrame &frame)
    {
        auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor || !(descriptor->flags & shared_flag) || !descriptor->references ||
            (descriptor->flags & pending_flag))
            return Status::invalid_argument("shared frame is foreign, stale or pending release");
        if (descriptor->references == max_count)
            return Status::overflow("shared frame reference count exhausted");
        ++descriptor->references;
        return Status::success();
    }
    Status release_reference(const memory::PhysicalFrame &frame)
    {
        auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor || !descriptor->references)
            return Status::invalid_argument("native user frame reference is foreign or stale");
#if defined(OK_ENABLE_TEST_POINTS)
        if (refuse_all_reference_releases_ || (refuse_reference_releases_ && same_frame(refused_reference_, frame)))
        {
            if (refuse_reference_releases_) --refuse_reference_releases_;
            return Status::fault("injected native pool reference release refusal");
        }
#endif
        if (descriptor->references > 1)
        {
            --descriptor->references;
            return Status::success();
        }
        // A retired table can still borrow this page after the last process
        // reference is gone. The pin is independent of the process count.
        if (descriptor->pins)
        {
            descriptor->references = 0;
            descriptor->flags |= pending_flag;
            ++pending_count_;
            return Status::success();
        }
        // Keep the last reference intact if the allocator rejects its
        // canonical handle, so the failed release remains diagnosable.
        return release_frame(frame);
    }
    Status release(uptr address)
    {
        const auto frame = find_user(address);
        if (!frame)
            return Status::not_found("native user frame has no retained owner");
        return release_reference(*frame);
    }
    Status pin(uptr address)
    {
        const auto frame = find_user(address);
        return frame ? pin(*frame) : Status::fault("invalid native frame quarantine pin");
    }
    // A waiter retains the exact allocator generation, never a mutable VM
    // record or an address that may later name a MAP_FIXED replacement.
    Status pin(const memory::PhysicalFrame &frame)
    {
        auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor || descriptor->pins == max_count || !descriptor->references ||
            (descriptor->flags & pending_flag))
            return Status::fault("invalid native frame quarantine pin");
        if (!descriptor->pins++)
            ++pinned_count_;
        return Status::success();
    }
    Status unpin(uptr address)
    {
        const auto frame = find_user(address);
        return frame ? unpin(*frame) : Status::fault("invalid native frame quarantine release");
    }
    Status unpin(const memory::PhysicalFrame &frame)
    {
        auto *descriptor = state(frame, memory::PageKind::native_user);
        if (!descriptor || !descriptor->pins)
            return Status::fault("invalid native frame quarantine release");
#if defined(OK_ENABLE_TEST_POINTS)
        if (refuse_unpins_ && same_frame(refused_unpin_, frame))
        {
            --refuse_unpins_;
            return Status::fault("injected native pool unpin refusal");
        }
#endif
        if (descriptor->pins == 1 && (descriptor->flags & pending_flag) && !descriptor->references)
            return release_frame(frame);
        if (!--descriptor->pins)
            --pinned_count_;
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
    [[nodiscard]] usize user_capacity_count() const { return user_limit_; }
    [[nodiscard]] usize pinned_count() const { return pinned_count_; }
    [[nodiscard]] usize pending_count() const { return pending_count_; }
    [[nodiscard]] usize reference_count(uptr address) const
    {
        const auto frame = find_user(address);
        return frame ? state(*frame, memory::PageKind::native_user)->references : 0;
    }
    [[nodiscard]] bool is_shared(uptr address) const
    {
        const auto frame = find_user(address);
        return frame && (state(*frame, memory::PageKind::native_user)->flags & shared_flag);
    }
    [[nodiscard]] usize peak_count() const { return peak_; }
    [[nodiscard]] u64 allocations() const { return allocations_; }
    [[nodiscard]] u64 releases() const { return releases_; }
    [[nodiscard]] std::optional<memory::PhysicalFrame> handle(uptr address) const { return find_user(address); }
  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct NativePagePoolFaultProbe;
#endif
    static constexpr u8 quarantined_flag = 1, pending_flag = 2, shared_flag = 4;
    // Reference and pin counts keep the established 16-bit bound.
    static constexpr u32 max_count = 65535;
    [[nodiscard]] static bool same_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
    {
        return left.owner == right.owner && left.index == right.index && left.generation == right.generation &&
               left.address == right.address;
    }
    [[nodiscard]] memory::PageDescriptor *state(const memory::PhysicalFrame &frame, memory::PageKind kind)
    {
        auto *descriptor = frames_ && frame.owner == frames_ ? frames_->descriptor(frame) : nullptr;
        return descriptor && descriptor->kind == static_cast<u8>(kind) ? descriptor : nullptr;
    }
    [[nodiscard]] const memory::PageDescriptor *state(const memory::PhysicalFrame &frame, memory::PageKind kind) const
    {
        const auto *descriptor = frames_ && frame.owner == frames_ ? frames_->descriptor(frame) : nullptr;
        return descriptor && descriptor->kind == static_cast<u8>(kind) ? descriptor : nullptr;
    }
    [[nodiscard]] bool live(const memory::PhysicalFrame &frame, memory::PageKind kind) const
    {
        return state(frame, kind) != nullptr;
    }
    Result<memory::PhysicalFrame> acquire(memory::PageKind kind)
    {
        if (!frames_)
            return Status::not_initialized("native page pool inactive");
        const bool user = kind == memory::PageKind::native_user;
        if ((user && user_count_ >= user_limit_) || (!user && object_count_ >= kernel_reserve_))
            return Status::no_memory("native page purpose quota exhausted");
        auto frame = frames_->allocate();
        if (!frame)
            return frame.status();
        auto *descriptor = frames_->descriptor(frame.value());
        if (!descriptor || descriptor->kind != static_cast<u8>(memory::PageKind::unowned))
        {
            static_cast<void>(frames_->release(frame.value()));
            return Status::fault("native frame registry collision");
        }
        descriptor->kind = static_cast<u8>(kind);
        if (user)
        {
            descriptor->references = 1;
            ++user_count_;
        }
        else ++object_count_;
        ++allocations_;
        const auto used = used_count();
        if (used > peak_) peak_ = used;
        return frame;
    }
    [[nodiscard]] std::optional<memory::PhysicalFrame> find_user(uptr address) const
    {
        if (!frames_)
            return std::nullopt;
        auto index = frames_->index_of(address);
        if (!index)
            return std::nullopt;
        auto frame = frames_->handle_at(index.value());
        if (!frame || !live(frame.value(), memory::PageKind::native_user))
            return std::nullopt;
        return frame.value();
    }
    Status release_frame(const memory::PhysicalFrame &frame)
    {
        const auto descriptor = *frames_->descriptor(frame);
        if (auto status = frames_->release(frame); !status)
            return status;
        if (descriptor.kind == static_cast<u8>(memory::PageKind::native_user)) --user_count_;
        else --object_count_;
        if (descriptor.flags & quarantined_flag) --quarantined_count_;
        if (descriptor.flags & pending_flag) --pending_count_;
        if (descriptor.pins) --pinned_count_;
        ++releases_;
        return Status::success();
    }
    memory::FrameAllocator *frames_{};
    Source source_{Source::inactive};
#if defined(OK_ENABLE_TEST_POINTS)
    memory::PhysicalFrame refused_reference_{};
    usize refuse_reference_releases_{};
    bool refuse_all_reference_releases_{};
    memory::PhysicalFrame refused_unpin_{};
    usize refuse_unpins_{};
#endif
    usize kernel_reserve_{}, user_limit_{}, user_count_{}, object_count_{}, peak_{};
    usize pinned_count_{}, pending_count_{}, quarantined_count_{};
    u64 allocations_{}, releases_{};
};
} // namespace ok::user::native
