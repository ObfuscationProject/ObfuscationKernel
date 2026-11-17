#pragma once

#include "ok/arch/execution.hpp"

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace ok::arch::execution
{
// Slots of a fixed table: one live space per process, one per quarantine
// slot (a process's space plus one replacement candidate) and one per CPU.
// The native runtime checks this sum against its own limits.
inline constexpr usize fixed_space_slots = 64 + (64 + 1) + 16;

// Address-space slots of one backend, kept in table storage. Generations come
// from one sequence that outlives the storage: a stale handle never matches a
// later space, even one whose root reuses the same frame.
//
// FixedSlots == 0: the slot table doubles when full and is released once its
// last space retires, so no slot count is fixed. Mutation is serialized by
// the caller (the kernel lock).
//
// FixedSlots > 0 (the profiles that run kernel code on several CPUs): the
// storage for FixedSlots entries is allocated by the first acquire() and
// never moves or goes back, so an entry's address stays valid for lookups
// that hold no lock; a full table refuses with would_block. Acquire, publish,
// retire and release are serialized by the backend's space lock.
//
// Lock-free lookups (find) run inside a window (ok/smp/bringup.hpp): an
// entry's root, generation, used and retiring words are written and read as
// atomics, and a retiring entry no longer validates. The writer marks the
// entry retiring before quiesce_root, so after it no window can still use the
// entry's space, and release() may clear it. The generation doubles as the
// entry's sequence: release() and acquire() change it first, behind a release
// fence, and find() rereads it last, so a lookup that read any word of a
// released or reused entry fails rather than mixing two lives of the slot.
// A released entry stays retiring until it is acquired again.
template <typename Space, usize FixedSlots = 0> class SpaceTable
{
  public:
    static constexpr bool fixed = FixedSlots != 0;

    struct Entry
    {
        Space space{};
        uptr root{};
        u32 generation{};
        bool used{};
        bool retiring{};
    };
    static_assert(std::is_trivially_copyable_v<Entry>);

    // A free entry, marked used with a fresh generation. Valid until the next
    // acquire() (a growable table) or its release() (a fixed one); publish()
    // its root before handing out the handle.
    [[nodiscard]] Result<Entry *> acquire()
    {
        if (live_ == capacity_)
        {
            if constexpr (fixed)
            {
                if (entries() != nullptr)
                    return Status::would_block("native address-space slots are exhausted");
            }
            if (auto status = grow(); !status) return status;
        }
        auto *entries = this->entries();
        for (usize index = 0; index < capacity_; ++index)
            if (!word(entries[index].used).load(std::memory_order_relaxed))
            {
                auto &entry = entries[index];
                word(entry.generation).store(next_generation_++, std::memory_order_relaxed);
                if (next_generation_ == 0) next_generation_ = 1;
                std::atomic_thread_fence(std::memory_order_release);
                entry.space = {};
                word(entry.root).store(0, std::memory_order_relaxed);
                word(entry.retiring).store(false, std::memory_order_relaxed);
                word(entry.used).store(true, std::memory_order_release);
                ++live_;
                return &entry;
            }
        return Status::fault("native address-space slot accounting is inconsistent");
    }
    // Makes a fully built entry findable under `root`.
    void publish(Entry &entry, uptr root)
    {
        word(entry.root).store(root, std::memory_order_release);
    }
    [[nodiscard]] AddressSpace handle(const Entry &entry) const
    {
        return {word(entry.root).load(std::memory_order_relaxed), static_cast<u32>(&entry - entries()),
                word(entry.generation).load(std::memory_order_relaxed)};
    }
    // The live, not retiring space a handle names; lock-free inside a window.
    [[nodiscard]] Space *find(const AddressSpace &space) const
    {
        auto *entries = this->entries();
        if (entries == nullptr || space.slot >= capacity() || !space.root || !space.generation) return nullptr;
        auto &entry = entries[space.slot];
        const auto generation = word(entry.generation).load(std::memory_order_acquire);
        if (generation != space.generation || !word(entry.used).load(std::memory_order_acquire) ||
            word(entry.root).load(std::memory_order_acquire) != space.root ||
            word(entry.retiring).load(std::memory_order_acquire))
            return nullptr;
        std::atomic_thread_fence(std::memory_order_acquire);
        return word(entry.generation).load(std::memory_order_relaxed) == generation ? &entry.space : nullptr;
    }
    // Writer order step 1 for a destroy: new windows no longer find the space;
    // quiesce_root then waits for the windows that found it earlier.
    Status retire(Space &space)
    {
        auto *entry = owned(space);
        if (!entry) return Status::invalid_argument("native address-space slot is foreign or free");
        word(entry->retiring).store(true, std::memory_order_release);
        return Status::success();
    }
    // Retires an entry; a growable table itself goes back once no space is live.
    Status release(Space &space)
    {
        auto *entry = owned(space);
        if (!entry)
            return Status::invalid_argument("native address-space slot is foreign or free");
        word(entry->generation).store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        word(entry->retiring).store(true, std::memory_order_relaxed);
        word(entry->used).store(false, std::memory_order_relaxed);
        word(entry->root).store(0, std::memory_order_relaxed);
        entry->space = {};
        if (--live_ || fixed) return Status::success();
        const auto pages = pages_;
        const auto address = reinterpret_cast<uptr>(entries_);
        entries_ = nullptr;
        capacity_ = pages_ = 0;
        return release_table_pages(address, pages);
    }
    template <typename F> void each(F &&visit) const
    {
        auto *entries = this->entries();
        for (usize index = 0; index < capacity_; ++index)
            if (word(entries[index].used).load(std::memory_order_relaxed))
                visit(entries[index].space, word(entries[index].root).load(std::memory_order_relaxed));
    }
    [[nodiscard]] usize live() const { return live_; }
    [[nodiscard]] usize pages() const { return pages_; }
    [[nodiscard]] usize capacity() const { return word(capacity_).load(std::memory_order_acquire); }

  private:
    static_assert(offsetof(Entry, space) == 0);

    template <typename T> [[nodiscard]] static std::atomic_ref<T> word(T &value) { return std::atomic_ref<T>{value}; }
    template <typename T> [[nodiscard]] static std::atomic_ref<T> word(const T &value)
    {
        return std::atomic_ref<T>{const_cast<T &>(value)};
    }
    [[nodiscard]] Entry *entries() const { return word(entries_).load(std::memory_order_acquire); }
    [[nodiscard]] Entry *owned(Space &space) const
    {
        auto *entry = reinterpret_cast<Entry *>(&space);
        auto *entries = this->entries();
        return entry >= entries && entry < entries + capacity_ && word(entry->used).load(std::memory_order_relaxed)
                   ? entry : nullptr;
    }

    Status grow()
    {
        usize pages;
        if constexpr (fixed)
            pages = (FixedSlots * sizeof(Entry) + page_size - 1) / page_size;
        else
        {
            pages = pages_ ? pages_ * 2 : 1;
            if (pages_ && pages / 2 != pages_) return Status::overflow("native address-space slot table overflow");
        }
        auto storage = allocate_table_pages(pages);
        if (!storage) return storage.status();
        auto *entries = reinterpret_cast<Entry *>(storage.value());
        const usize capacity = fixed ? FixedSlots : pages * page_size / sizeof(Entry);
        for (usize index = 0; index < capacity; ++index)
            new (&entries[index]) Entry{index < capacity_ ? entries_[index] : Entry{}};
        const auto previous = reinterpret_cast<uptr>(entries_);
        const auto previous_pages = pages_;
        // A fixed table publishes its storage once, for lock-free lookups.
        word(capacity_).store(capacity, std::memory_order_relaxed);
        word(entries_).store(entries, std::memory_order_release);
        pages_ = pages;
        // A failed release leaves the old run unreachable but accounted to
        // its owner; the grown table is already authoritative.
        return previous_pages ? release_table_pages(previous, previous_pages) : Status::success();
    }

    Entry *entries_{};
    usize capacity_{}, pages_{}, live_{};
    u32 next_generation_{1};
};
} // namespace ok::arch::execution
