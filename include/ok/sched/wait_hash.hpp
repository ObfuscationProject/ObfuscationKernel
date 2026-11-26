#pragma once

// Wait channels (smp-scaling WP4b, spec 4.3). A wait names the channels that
// can make it ready, each a (reason, key) pair such as (pipe_read, pipe); a
// waker making a channel's condition true completes the waits linked there.
// Entries live in a fixed pool and hash into a fixed number of buckets, each
// with its own lock, so a wake visits one short list instead of every thread.
//
// Every entry carries a generation. Whoever unlinks an entry (a waker that
// takes it, or its owner dropping what is left after its wait completed some
// other way) bumps the generation under the bucket lock and then frees it, so
// an owner's handle names an entry only as long as that same insertion is
// linked. No entry outlives the wait it was made for once its owner runs.

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/run_queue.hpp"
#include "ok/sched/wait_registration.hpp"

#include <array>
#include <atomic>
#include <optional>
#include <span>

namespace ok::sched
{

// One linked entry as its owner knows it.
struct WaitHashHandle
{
    u16 index{0xffff};
    u8 bucket{};
    u32 generation{};

    [[nodiscard]] constexpr bool valid() const { return index != 0xffff; }
    [[nodiscard]] constexpr bool operator==(const WaitHashHandle &) const = default;
};

// `Lock` is the bucket lock: the ranked wait_hash spin lock in the kernel, a
// real lock in the hosted race models. Pool slots are claimed with a word
// exchange and released with a store, so no lock guards the pool and i386
// needs no compare-exchange.
template <usize Buckets, usize Entries, RunQueueLock Lock> class WaitHash final : private NonCopyable
{
    static_assert(Buckets != 0 && Buckets <= 256, "a handle names its bucket in one byte");
    static_assert(Entries != 0 && Entries < 0xffff, "entry indices are u16 with 0xffff for none");

  public:
    // A wake unlinks at most this many entries per bucket pass (spec A2).
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

    // Links `ticket` on the channel (reason, key). Empty when the pool is
    // exhausted; the caller then makes its wait re-evaluate on a timer.
    [[nodiscard]] std::optional<WaitHashHandle> insert(u8 reason, u64 key, WaitTicket ticket)
    {
        const u16 index = claim();
        if (index == none)
        {
            exhausted_.store(exhausted_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
        auto &entry = entries_[index];
        const auto bucket = static_cast<u8>(bucket_of(reason, key));
        auto &list = buckets_[bucket];
        list.lock.lock();
        entry.reason = reason;
        entry.key = key;
        entry.ticket = ticket;
        entry.next = none;
        if (list.tail == none) list.head = index;
        else entries_[list.tail].next = index;
        list.tail = index;
        const WaitHashHandle handle{index, bucket, entry.generation.load(std::memory_order_relaxed)};
        list.lock.unlock();
        return handle;
    }

    // The owner drops an entry it linked; false when a waker took it first
    // (or it was dropped already). The generation decides: it moves on every
    // unlink, and a reused entry was unlinked before it was claimed again.
    bool unlink(WaitHashHandle handle)
    {
        if (!handle.valid() || handle.index >= Entries || handle.bucket >= Buckets) return false;
        auto &list = buckets_[handle.bucket];
        list.lock.lock();
        auto &entry = entries_[handle.index];
        if (entry.generation.load(std::memory_order_relaxed) != handle.generation)
        {
            list.lock.unlock();
            return false;
        }
        u16 previous = none;
        u16 at = list.head;
        while (at != none && at != handle.index)
        {
            previous = at;
            at = entries_[at].next;
        }
        if (at == none)
        {
            // Matching generation but not linked here: a broken handle. Leave
            // the entry alone rather than free one another list holds.
            list.lock.unlock();
            return false;
        }
        detach(list, previous, at);
        list.lock.unlock();
        release(handle.index);
        return true;
    }

    // A2: unlinks up to wake_batch entries of the channel into `out` (their
    // tickets, oldest first). The caller completes them after this returns,
    // holding no bucket lock, and takes again while `more` is set.
    Taken take(u8 reason, u64 key, std::span<WaitTicket, wake_batch> out)
    {
        auto &list = buckets_[bucket_of(reason, key)];
        std::array<u16, wake_batch> freed{};
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
            out[taken.count] = entry.ticket;
            freed[taken.count++] = at;
            detach(list, previous, at);
            at = next;
        }
        list.lock.unlock();
        for (usize i = 0; i < taken.count; ++i) release(freed[i]);
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
    // Pool slots claimed now: linked entries plus inserts and unlinks in flight.
    [[nodiscard]] usize claimed() const
    {
        usize count = 0;
        for (const auto &entry : entries_) count += entry.claimed.load(std::memory_order_relaxed) != 0;
        return count;
    }
    // Inserts refused for want of an entry.
    [[nodiscard]] u64 exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

  private:
    struct Entry
    {
        std::atomic<u32> claimed{};
        // Stored under the bucket lock that links the entry; the owner's
        // unlink loads it under the lock its handle names.
        std::atomic<u32> generation{};
        u64 key{};
        WaitTicket ticket{};
        u16 next{none};
        u8 reason{};
    };
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
    void release(u16 index) { entries_[index].claimed.store(0, std::memory_order_release); }
    // Caller holds the bucket lock.
    void detach(Bucket &list, u16 previous, u16 index)
    {
        auto &entry = entries_[index];
        if (previous == none) list.head = entry.next;
        else entries_[previous].next = entry.next;
        if (list.tail == index) list.tail = previous;
        entry.next = none;
        entry.generation.store(entry.generation.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    std::array<Bucket, Buckets> buckets_{};
    std::array<Entry, Entries> entries_{};
    std::atomic<u32> cursor_{};
    std::atomic<u32> exhausted_{};
};

} // namespace ok::sched
