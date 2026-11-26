#pragma once

// Wait channels (smp-scaling WP4b, spec 4.3). A wait names the channels that
// can make it ready, each a (reason, key) pair such as (pipe_read, pipe); a
// waker making a channel's condition true completes the waits linked there.
// Entries live in a fixed pool and hash into a fixed number of buckets, each
// with its own lock, so a wake visits one short list instead of every thread.
//
// An entry belongs to the wait that inserted it until that wait's owner
// releases it: a waker only detaches it from its bucket (and completes the
// wait after dropping the bucket lock), the owner detaches what is still
// linked and frees every entry it inserted. Since only the owner frees, an
// entry index stays the owner's until it lets go, and the owner can chain
// its entries through them (`owner_next`) instead of keeping handles.

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/run_queue.hpp"

#include <array>
#include <atomic>
#include <span>

namespace ok::sched
{

// What a linked entry remembers of its wait: the waiter's TCB slot and tid,
// and the low 32 bits of its registration generation. A completion turns it
// back into the full ticket under the waiter's task lock.
struct WaitHashTicket
{
    u32 thread{};
    u32 generation{};
    u16 index{0xffff};

    [[nodiscard]] constexpr bool operator==(const WaitHashTicket &) const = default;
};

// `Lock` is the bucket lock: the ranked wait_hash spin lock in the kernel, a
// real lock in the hosted race models. Pool slots are claimed with a word
// exchange and released with a store, so no lock guards the pool and i386
// needs no compare-exchange.
template <usize Buckets, usize Entries, RunQueueLock Lock> class WaitHash final : private NonCopyable
{
    static_assert(Buckets != 0 && Buckets <= 256, "an entry names its bucket in one byte");
    static_assert(Entries != 0 && Entries < 0xffff, "entry indices are u16 with 0xffff for none");

  public:
    // A wake detaches at most this many entries per bucket pass (spec A2).
    static constexpr usize wake_batch = 16;
    static constexpr u16 none = 0xffff;

    struct Taken
    {
        usize count{};
        bool more{}; // The bucket still holds a match: take again.
    };

    constexpr WaitHash() = default;

    [[nodiscard]] static constexpr usize capacity() { return Entries; }
    [[nodiscard]] static constexpr usize bucket_count() { return Buckets; }
    [[nodiscard]] static constexpr usize bucket_of(u8 reason, u64 key)
    {
        u64 mixed = key * 0x9e3779b97f4a7c15ULL ^ (u64{reason} + 1) * 0xc2b2ae3d27d4eb4fULL;
        mixed ^= mixed >> 29;
        mixed *= 0xbf58476d1ce4e5b9ULL;
        mixed ^= mixed >> 32;
        return static_cast<usize>(mixed % Buckets);
    }

    // Links `ticket` on the channel (reason, key), chained before
    // `owner_next` in its owner's list; the entry's index, or none when the
    // pool is exhausted (the caller then makes its wait re-evaluate on a timer).
    [[nodiscard]] u16 insert(u8 reason, u64 key, WaitHashTicket ticket, u16 owner_next = none)
    {
        const u16 index = claim();
        if (index == none)
        {
            exhausted_.store(exhausted_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return none;
        }
        auto &entry = entries_[index];
        const auto bucket = static_cast<u8>(bucket_of(reason, key));
        entry.reason = reason;
        entry.bucket = bucket;
        entry.key = key;
        entry.ticket = ticket;
        entry.owner_next = owner_next;
        entry.next = none;
        auto &list = buckets_[bucket];
        list.lock.lock();
        if (list.tail == none) list.head = index;
        else entries_[list.tail].next = index;
        list.tail = index;
        entry.linked = true;
        list.lock.unlock();
        return index;
    }

    // The owner lets an entry go: detached if a waker did not take it, then
    // freed. Returns whether it was still linked; `owner_next` is the next
    // entry of the owner's chain.
    bool release(u16 index, u16 &owner_next)
    {
        owner_next = none;
        if (index >= Entries || entries_[index].claimed.load(std::memory_order_relaxed) == 0) return false;
        auto &entry = entries_[index];
        owner_next = entry.owner_next;
        auto &list = buckets_[entry.bucket];
        list.lock.lock();
        const bool linked = entry.linked;
        if (linked)
        {
            u16 previous = none;
            u16 at = list.head;
            while (at != none && at != index)
            {
                previous = at;
                at = entries_[at].next;
            }
            if (at == index) detach(list, previous, index);
        }
        list.lock.unlock();
        entry.claimed.store(0, std::memory_order_release);
        return linked;
    }
    bool release(u16 index)
    {
        u16 next{};
        return release(index, next);
    }

    // A2: detaches up to wake_batch entries of the channel and copies their
    // tickets to `out`, oldest first. The caller completes them after this
    // returns, holding no bucket lock, and takes again while `more` is set.
    // The entries stay claimed until their owners release them.
    Taken take(u8 reason, u64 key, std::span<WaitHashTicket, wake_batch> out)
    {
        auto &list = buckets_[bucket_of(reason, key)];
        Taken taken{};
        list.lock.lock();
        u16 previous = none;
        u16 at = list.head;
        while (at != none)
        {
            auto &entry = entries_[at];
            const u16 next = entry.next;
            if (entry.reason != reason || entry.key != key)
            {
                previous = at;
                at = next;
                continue;
            }
            if (taken.count == wake_batch)
            {
                taken.more = true;
                break;
            }
            out[taken.count++] = entry.ticket;
            detach(list, previous, at);
            at = next;
        }
        list.lock.unlock();
        return taken;
    }

    // Entries of the channel linked now (tests and audits; takes the bucket).
    [[nodiscard]] usize linked(u8 reason, u64 key)
    {
        auto &list = buckets_[bucket_of(reason, key)];
        usize count = 0;
        list.lock.lock();
        for (u16 at = list.head; at != none; at = entries_[at].next)
            count += entries_[at].reason == reason && entries_[at].key == key;
        list.lock.unlock();
        return count;
    }
    // Pool slots claimed now: linked entries plus detached ones whose owners
    // have not released them yet.
    [[nodiscard]] usize claimed() const
    {
        usize count = 0;
        for (const auto &entry : entries_) count += entry.claimed.load(std::memory_order_relaxed) != 0;
        return count;
    }
    // Inserts refused for want of an entry.
    [[nodiscard]] u64 exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

  private:
    // 32 bytes: the pool sits in every scheduler instance.
    struct Entry
    {
        std::atomic<u32> claimed{};
        WaitHashTicket ticket{};
        u64 key{};
        u16 next{none};       // Bucket list, under the bucket lock.
        u16 owner_next{none}; // The owner's chain, the owner's alone.
        u8 reason{};
        u8 bucket{};
        bool linked{}; // Under the bucket lock.
    };
    static_assert(sizeof(Entry) <= 32);
    struct Bucket
    {
        Lock lock{};
        u16 head{none};
        u16 tail{none};
    };

    u16 claim()
    {
        const usize start = cursor_.load(std::memory_order_relaxed) % Entries;
        for (usize offset = 0; offset < Entries; ++offset)
        {
            const usize index = (start + offset) % Entries;
            auto &entry = entries_[index];
            if (entry.claimed.load(std::memory_order_relaxed) == 0 &&
                entry.claimed.exchange(1, std::memory_order_acquire) == 0)
            {
                cursor_.store(static_cast<u32>((index + 1) % Entries), std::memory_order_relaxed);
                return static_cast<u16>(index);
            }
        }
        return none;
    }
    // Caller holds the bucket lock.
    void detach(Bucket &list, u16 previous, u16 index)
    {
        auto &entry = entries_[index];
        if (previous == none) list.head = entry.next;
        else entries_[previous].next = entry.next;
        if (list.tail == index) list.tail = previous;
        entry.next = none;
        entry.linked = false;
    }

    std::array<Bucket, Buckets> buckets_{};
    std::array<Entry, Entries> entries_{};
    std::atomic<u32> cursor_{};
    std::atomic<u32> exhausted_{};
};

} // namespace ok::sched
