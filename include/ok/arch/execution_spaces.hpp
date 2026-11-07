#pragma once

#include "ok/arch/execution.hpp"

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
// that hold no lock; a full table refuses with would_block. Acquire and
// release are serialized by the backend's space lock.
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
    };
    static_assert(std::is_trivially_copyable_v<Entry>);

    // A free entry, marked used with a fresh generation. Valid until the next
    // acquire() (a growable table) or its release() (a fixed one); publish
    // its root before handing out the handle.
    [[nodiscard]] Result<Entry *> acquire()
    {
        if (live_ == capacity_)
        {
            if constexpr (fixed)
            {
                if (entries_ != nullptr)
                    return Status::would_block("native address-space slots are exhausted");
            }
            if (auto status = grow(); !status) return status;
        }
        for (usize index = 0; index < capacity_; ++index)
            if (!entries_[index].used)
            {
                auto &entry = entries_[index];
                entry = {};
                entry.used = true;
                entry.generation = next_generation_++;
                if (next_generation_ == 0) next_generation_ = 1;
                ++live_;
                return &entry;
            }
        return Status::fault("native address-space slot accounting is inconsistent");
    }
    [[nodiscard]] AddressSpace handle(const Entry &entry) const
    {
        return {entry.root, static_cast<u32>(&entry - entries_), entry.generation};
    }
    [[nodiscard]] Space *find(const AddressSpace &space) const
    {
        if (space.slot >= capacity_) return nullptr;
        auto &entry = entries_[space.slot];
        return entry.used && entry.root && entry.root == space.root && entry.generation == space.generation
                   ? &entry.space : nullptr;
    }
    // Retires an entry; a growable table itself goes back once no space is live.
    Status release(Space &space)
    {
        auto *entry = reinterpret_cast<Entry *>(&space);
        if (entry < entries_ || entry >= entries_ + capacity_ || !entry->used)
            return Status::invalid_argument("native address-space slot is foreign or free");
        *entry = {};
        if (--live_ || fixed) return Status::success();
        const auto pages = pages_;
        const auto address = reinterpret_cast<uptr>(entries_);
        entries_ = nullptr;
        capacity_ = pages_ = 0;
        return release_table_pages(address, pages);
    }
    template <typename F> void each(F &&visit) const
    {
        for (usize index = 0; index < capacity_; ++index)
            if (entries_[index].used) visit(entries_[index].space, entries_[index].root);
    }
    [[nodiscard]] usize live() const { return live_; }
    [[nodiscard]] usize pages() const { return pages_; }
    [[nodiscard]] usize capacity() const { return capacity_; }

  private:
    static_assert(offsetof(Entry, space) == 0);

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
        entries_ = entries;
        capacity_ = capacity;
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
