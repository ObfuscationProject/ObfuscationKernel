#pragma once

// Futex hash buckets (smp-scaling spec 4.5). Each bucket is a futex_bucket
// spin lock (class A, rank 42) over a FIFO list of queued waits, linked
// through the waits themselves, and a queue sequence that numbers each wait
// as it is queued. A pair of buckets is taken in index order, the higher one
// through lock_as<futex_bucket_second>; an equal pair is taken once.
//
// A wait's `bucket` word names the bucket that lists it, or none. It changes
// only with that bucket's lock held (a move holds both), by store; its owner
// loads it without a lock to find the bucket to take, then checks it again
// under that lock. Unlinking publishes none last, with release order: once a
// waker has unlinked a wait it touches the record no more, so an owner that
// loads none may retire the record at once. A move never publishes none, so
// an owner never retires a wait that a requeue is still relinking.
#include "ok/core/types.hpp"
#include "ok/smp/lock.hpp"

#include <array>
#include <bit>
#include <concepts>

namespace ok::user::native
{
inline constexpr usize futex_bucket_count = 64;
inline constexpr u32 futex_no_bucket = ~u32{0};

// The link fields a queued wait carries (FutexWait, and the hosted models).
template <typename Wait>
concept FutexQueued = requires(Wait &wait) {
    { wait.bucket_prev } -> std::same_as<Wait *&>;
    { wait.bucket_next } -> std::same_as<Wait *&>;
    { wait.bucket } -> std::same_as<u32 &>;
    { wait.queue_sequence } -> std::same_as<u64 &>;
};

template <typename Lock>
concept FutexBucketLock = smp::RankedLock<Lock> && requires(Lock &lock) {
    lock.template lock_as<smp::LockRank::futex_bucket_second>();
};

template <FutexQueued Wait, FutexBucketLock Lock, usize Count = futex_bucket_count> class FutexBuckets final
{
    static_assert(std::has_single_bit(Count) && Count <= (usize{1} << 16), "bucket count is a small power of two");

  public:
    static constexpr u32 none = futex_no_bucket;
    static constexpr usize count = Count;

    struct Bucket
    {
        Lock lock{};
        Wait *head{};
        Wait *tail{};
        // Mutated under the lock only (no 64-bit atomic on 32-bit profiles).
        u64 sequence{};
    };

    // A key's bucket from a 32-bit digest of the key (Fibonacci hashing).
    [[nodiscard]] static constexpr u32 index_of(u32 digest) noexcept
    {
        constexpr unsigned shift = 32U - static_cast<unsigned>(std::countr_zero(Count));
        if constexpr (Count == 1)
            return 0;
        else
            return static_cast<u32>((digest * 0x9e3779b9U) >> shift);
    }

    [[nodiscard]] Bucket &operator[](u32 index) noexcept
    {
        return buckets_[index];
    }

    // The bucket listing `wait`, or none; the owner's lock-free hint.
    [[nodiscard]] static u32 linked_bucket(const Wait &wait) noexcept
    {
        return __atomic_load_n(&wait.bucket, __ATOMIC_ACQUIRE);
    }

    // The remaining functions up to lock_linked run with `index`'s lock held
    // (both locks for move).

    // Whether `wanted` more sequences fit before the bucket's sequence wraps.
    [[nodiscard]] bool sequences_left(u32 index, usize wanted) const noexcept
    {
        return wanted <= ~u64{0} - buckets_[index].sequence;
    }

    // Queues `wait` last and numbers it; the caller checked sequences_left.
    void link_tail(u32 index, Wait &wait) noexcept
    {
        auto &bucket = buckets_[index];
        wait.queue_sequence = ++bucket.sequence;
        append(bucket, wait);
        __atomic_store_n(&wait.bucket, index, __ATOMIC_RELEASE);
    }

    // Takes `wait` off the list. After this the caller must not touch the
    // record: its owner may retire it as soon as it sees none.
    void unlink(u32 index, Wait &wait) noexcept
    {
        detach(buckets_[index], wait);
        __atomic_store_n(&wait.bucket, none, __ATOMIC_RELEASE);
    }

    // Requeue: `wait` leaves `from` and is queued last on `to`, numbered by
    // `to`'s sequence (checked with sequences_left). Holding both locks.
    void move(u32 from, u32 to, Wait &wait) noexcept
    {
        detach(buckets_[from], wait);
        auto &target = buckets_[to];
        wait.queue_sequence = ++target.sequence;
        append(target, wait);
        __atomic_store_n(&wait.bucket, to, __ATOMIC_RELEASE);
    }

    // The owner's side: locks the bucket that lists `wait` and returns its
    // index, following a concurrent move; returns none, holding nothing,
    // once the wait is off every list. The caller holds no bucket.
    [[nodiscard]] u32 lock_linked(const Wait &wait) noexcept
    {
        for (;;)
        {
            const auto index = linked_bucket(wait);
            if (index == none)
                return none;
            buckets_[index].lock.lock();
            if (__atomic_load_n(&wait.bucket, __ATOMIC_RELAXED) == index)
                return index;
            buckets_[index].lock.unlock();
        }
    }

    // One bucket for a scope.
    class Guard final
    {
      public:
        Guard(FutexBuckets &buckets, u32 index) noexcept : lock_(buckets.buckets_[index].lock)
        {
            lock_.lock();
        }
        ~Guard()
        {
            lock_.unlock();
        }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

      private:
        Lock &lock_;
    };

    // Two buckets for a scope, in index order; an equal pair is taken once.
    class PairGuard final
    {
      public:
        PairGuard(FutexBuckets &buckets, u32 first, u32 second) noexcept
            : lower_(buckets.buckets_[first < second ? first : second].lock),
              higher_(first == second ? nullptr : &buckets.buckets_[first < second ? second : first].lock)
        {
            lower_.lock();
            if (higher_)
                higher_->template lock_as<smp::LockRank::futex_bucket_second>();
        }
        ~PairGuard()
        {
            if (higher_)
                higher_->unlock();
            lower_.unlock();
        }
        PairGuard(const PairGuard &) = delete;
        PairGuard &operator=(const PairGuard &) = delete;

      private:
        Lock &lower_;
        Lock *higher_;
    };

  private:
    static void append(Bucket &bucket, Wait &wait) noexcept
    {
        wait.bucket_prev = bucket.tail;
        wait.bucket_next = nullptr;
        if (bucket.tail)
            bucket.tail->bucket_next = &wait;
        else
            bucket.head = &wait;
        bucket.tail = &wait;
    }

    static void detach(Bucket &bucket, Wait &wait) noexcept
    {
        if (wait.bucket_prev)
            wait.bucket_prev->bucket_next = wait.bucket_next;
        else
            bucket.head = wait.bucket_next;
        if (wait.bucket_next)
            wait.bucket_next->bucket_prev = wait.bucket_prev;
        else
            bucket.tail = wait.bucket_prev;
        wait.bucket_prev = wait.bucket_next = nullptr;
    }

    std::array<Bucket, Count> buckets_{};
};
} // namespace ok::user::native
