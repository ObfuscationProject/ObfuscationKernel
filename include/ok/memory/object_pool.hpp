#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <concepts>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace ok::memory
{

struct ObjectHandle
{
    const void *pool{};
    u32 page_slot{};
    u32 object_slot{};
    u64 generation{};

    [[nodiscard]] constexpr bool operator==(const ObjectHandle &) const = default;
};

struct ObjectAccounting
{
    usize live_objects{};
    usize backing_pages{};
    usize quarantined_pages{};
    usize empty_pages{};
    usize peak_objects{};
    u64 allocations{};
    u64 destructions{};
};

template <class P>
concept ObjectPageProvider = requires(P &provider, typename P::Lease lease) {
    { P::page_size } -> std::convertible_to<usize>;
    { lease.bytes } -> std::same_as<std::span<std::byte> &>;
    { provider.allocate_kernel_page() } -> std::same_as<Result<typename P::Lease>>;
    { provider.release_kernel_page(lease) } -> std::same_as<Status>;
} && std::is_nothrow_default_constructible_v<typename P::Lease> &&
    std::is_nothrow_destructible_v<typename P::Lease> &&
    std::is_nothrow_move_constructible_v<typename P::Lease> &&
    std::is_nothrow_move_assignable_v<typename P::Lease>;

// The provider owns mapping authority and exact page leases. One serialized
// boot-CPU caller owns this pool; IRQ/SMP access requires a separate contract.
// Pool and provider must outlive all objects/handles/leases. Shutdown drains
// object users, destroys each object, then explicitly trims empty pages.
template <class T, ObjectPageProvider P, usize MaximumPages>
    requires(std::is_nothrow_destructible_v<T> && sizeof(T) <= P::page_size &&
             alignof(T) <= P::page_size && MaximumPages > 0)
class ObjectPool final : private NonCopyable
{
  public:
    static constexpr usize slots_per_page = P::page_size / sizeof(T);
    static constexpr usize max_pages = MaximumPages;
    static_assert((P::page_size & (P::page_size - 1)) == 0, "object page size must be a power of two");
    static_assert(MaximumPages <= ~u32{0} && slots_per_page <= ~u32{0});
    static_assert(MaximumPages <= ~usize{0} / slots_per_page);
    static constexpr usize capacity = MaximumPages * slots_per_page;

    constexpr explicit ObjectPool(P &provider, usize max_objects = capacity) : provider_(provider), limit_(max_objects) {}

    template <class... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    Result<ObjectHandle> create(Args &&...arguments)
    {
        if (mutating_)
            return Status::busy("object pool mutation is not reentrant");
        Mutation mutation{mutating_};
        if (!fault_)
            return fault_;
        if (limit_ == 0 || limit_ > capacity)
            return Status::invalid_argument("object pool live limit is invalid");
        if (live_ >= limit_)
            return Status::no_memory("object pool live object limit reached");
        if (generation_ == ~u64{0})
            return Status::overflow("object handle generation exhausted");

        usize page = MaximumPages;
        for (usize i = 0; i < MaximumPages; ++i)
            if (pages_[i].state == PageState::ready && pages_[i].live < slots_per_page)
            {
                page = i;
                break;
            }
        if (page == MaximumPages)
        {
            for (usize i = 0; i < MaximumPages; ++i)
                if (pages_[i].state == PageState::empty)
                {
                    page = i;
                    break;
                }
            if (page == MaximumPages)
                return Status::no_memory("object pool backing page limit reached");
            auto lease = provider_.allocate_kernel_page();
            if (!lease)
                return lease.status();
            auto &record = pages_[page];
            record.lease = std::move(lease.value());
            record.state = PageState::quarantined;
            const auto bytes = record.lease.bytes;
            const uptr address = reinterpret_cast<uptr>(bytes.data());
            bool overlap = false;
            for (usize i = 0; i < MaximumPages; ++i)
                if (i != page && pages_[i].state != PageState::empty)
                {
                    const auto other = pages_[i].lease.bytes;
                    const uptr start = reinterpret_cast<uptr>(other.data());
                    // Difference arithmetic avoids evaluating a wrapping end.
                    overlap |= address >= start ? address - start < other.size() : start - address < bytes.size();
                }
            if (overlap)
            {
                // Releasing an aliased token could retire a live object's page.
                // Retain both records without dereferencing the suspect bytes.
                fault_ = Status::fault("object provider returned overlapping page leases");
                return fault_;
            }
            if (address == 0 || bytes.size() != P::page_size || address % P::page_size != 0 ||
                bytes.size() - 1 > ~uptr{0} - address)
            {
                fault_ = Status::fault("object provider returned invalid supervisor page storage");
                if (auto released = provider_.release_kernel_page(record.lease); released)
                    record = {};
                return fault_;
            }
            for (auto &byte : bytes)
                byte = std::byte{};
            record.state = PageState::ready;
        }
        auto &record = pages_[page];
        for (usize slot = 0; slot < slots_per_page; ++slot)
            if (record.generations[slot] == 0)
            {
                auto *storage = record.lease.bytes.data() + slot * sizeof(T);
                for (usize i = 0; i < sizeof(T); ++i)
                    storage[i] = std::byte{};
                ::new (static_cast<void *>(storage)) T(std::forward<Args>(arguments)...);
                const u64 generation = ++generation_;
                record.generations[slot] = generation;
                ++record.live;
                ++live_;
                if (live_ > peak_)
                    peak_ = live_;
                return ObjectHandle{this, static_cast<u32>(page), static_cast<u32>(slot), generation};
            }
        // Counts and slot occupancy are private and changed together.
        fault_ = Status::fault("object pool occupancy invariant failed");
        return fault_;
    }

    Result<T *> resolve(ObjectHandle handle)
    {
        if (auto status = validate(handle); !status)
            return status;
        return object(pages_[handle.page_slot], handle.object_slot);
    }

    Result<const T *> resolve(ObjectHandle handle) const
    {
        if (auto status = validate(handle); !status)
            return status;
        return object(pages_[handle.page_slot], handle.object_slot);
    }

    Status destroy(ObjectHandle handle)
    {
        if (auto status = validate(handle); !status)
            return status;
        Mutation mutation{mutating_};
        auto &record = pages_[handle.page_slot];
        auto *value = object(record, handle.object_slot);
        record.generations[handle.object_slot] = 0;
        value->~T();
        auto *bytes = record.lease.bytes.data() + handle.object_slot * sizeof(T);
        for (usize i = 0; i < sizeof(T); ++i)
            bytes[i] = std::byte{};
        --record.live;
        --live_;
        ++destructions_;
        return Status::success();
    }

    Status trim_empty_pages()
    {
        if (mutating_)
            return Status::busy("object pool mutation is not reentrant");
        Mutation mutation{mutating_};
        for (auto &page : pages_)
            if (page.state == PageState::ready && page.live == 0)
            {
                if (auto status = provider_.release_kernel_page(page.lease); !status)
                {
                    page.state = PageState::quarantined;
                    if (fault_)
                        fault_ = status;
                }
                else
                    page = {};
            }
        return fault_;
    }

    [[nodiscard]] Status ownership_status() const
    {
        return mutating_ ? Status::busy("object pool mutation is active") : fault_;
    }

    [[nodiscard]] ObjectAccounting accounting() const
    {
        ObjectAccounting result{.live_objects = live_, .peak_objects = peak_,
                                .allocations = generation_, .destructions = destructions_};
        for (const auto &page : pages_)
        {
            result.backing_pages += page.state != PageState::empty;
            result.quarantined_pages += page.state == PageState::quarantined;
            result.empty_pages += page.state == PageState::ready && page.live == 0;
        }
        return result;
    }

  private:
    struct Mutation
    {
        explicit Mutation(bool &active) : active_(active) { active_ = true; }
        ~Mutation() { active_ = false; }
        bool &active_;
    };
    enum class PageState : u8 { empty, ready, quarantined };
    struct Page
    {
        typename P::Lease lease{};
        std::array<u64, slots_per_page> generations{};
        usize live{};
        PageState state{PageState::empty};
    };

    [[nodiscard]] Status validate(ObjectHandle handle) const
    {
        if (mutating_)
            return Status::busy("object pool mutation is not reentrant");
        if (handle.pool != this || handle.page_slot >= MaximumPages || handle.object_slot >= slots_per_page ||
            handle.generation == 0)
            return Status::invalid_argument("object handle owner or index is invalid");
        const auto &record = pages_[handle.page_slot];
        if (record.state != PageState::ready || record.generations[handle.object_slot] != handle.generation)
            return Status::not_found("object handle is stale");
        return Status::success();
    }
    static T *object(Page &page, usize slot)
    {
        return std::launder(reinterpret_cast<T *>(page.lease.bytes.data() + slot * sizeof(T)));
    }
    static const T *object(const Page &page, usize slot)
    {
        return std::launder(reinterpret_cast<const T *>(page.lease.bytes.data() + slot * sizeof(T)));
    }

    friend struct ObjectPoolTestAccess;
    P &provider_;
    usize limit_{};
    std::array<Page, MaximumPages> pages_{};
    usize live_{}, peak_{};
    u64 generation_{}, destructions_{};
    bool mutating_{};
    Status fault_{};
};

} // namespace ok::memory
