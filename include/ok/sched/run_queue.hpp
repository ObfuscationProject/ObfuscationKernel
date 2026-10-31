#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <optional>

namespace ok::sched
{

using ProcessId = u64;
using ThreadId = u64;

// Placement records carry identities, never PCB/TCB addresses: process storage
// compacts on reap, while task identities are never reused.
struct ThreadRef
{
    ProcessId pid{};
    ThreadId tid{};

    [[nodiscard]] constexpr bool operator==(const ThreadRef &) const = default;
};

template <typename T>
concept RunQueueLock = requires(T &lock) {
    lock.lock();
    lock.unlock();
    { lock.try_lock() } -> std::same_as<bool>;
};

struct AnyThreadRef
{
    [[nodiscard]] constexpr bool operator()(ThreadRef) const { return true; }
};

// Every operation takes only this queue's lock and releases it before
// returning, so a caller never holds two queue locks. `try_take_first` is the
// stealing edge: it never spins on a contended victim.
template <typename Q>
concept RunQueue = requires(Q &queue, const Q &view, ThreadRef ref, AnyThreadRef any) {
    { queue.push(ref) } -> std::same_as<Status>;
    { queue.pop() } -> std::same_as<std::optional<ThreadRef>>;
    { queue.remove(ref) } -> std::same_as<bool>;
    { queue.try_take_first(any) } -> std::same_as<std::optional<ThreadRef>>;
    { view.contains(ref) } -> std::same_as<bool>;
    { view.size() } -> std::same_as<usize>;
};

// Bounded FIFO of thread identities owned by one CPU.
template <usize Capacity, RunQueueLock Lock = smp::SpinLock> class CpuRunQueue final
{
    static_assert(Capacity != 0);

  public:
    constexpr CpuRunQueue() = default;
    CpuRunQueue(const CpuRunQueue &) = delete;
    CpuRunQueue &operator=(const CpuRunQueue &) = delete;

    [[nodiscard]] static constexpr usize capacity() { return Capacity; }

    Status push(ThreadRef ref)
    {
        Guard guard{lock_};
        const auto count = count_.load(std::memory_order_relaxed);
        if (count == Capacity)
            return Status::overflow("CPU run queue is full");
        entries_[slot(count)] = ref;
        count_.store(count + 1, std::memory_order_relaxed);
        return {};
    }

    std::optional<ThreadRef> pop()
    {
        Guard guard{lock_};
        if (count_.load(std::memory_order_relaxed) == 0)
            return std::nullopt;
        return take(0);
    }

    // O(n); the remaining entries keep their FIFO order.
    bool remove(ThreadRef ref)
    {
        Guard guard{lock_};
        const auto index = find(ref);
        if (index == Capacity)
            return false;
        static_cast<void>(take(index));
        return true;
    }

    // Removes the oldest entry accepted by `eligible`, which runs under this
    // queue's lock. Returns nothing when the lock is contended.
    template <typename Predicate>
        requires std::predicate<Predicate &, ThreadRef>
    std::optional<ThreadRef> try_take_first(Predicate &&eligible)
    {
        if (!lock_.try_lock())
            return std::nullopt;
        std::optional<ThreadRef> taken{};
        const auto count = count_.load(std::memory_order_relaxed);
        for (usize index = 0; index < count; ++index)
            if (eligible(entries_[slot(index)]))
            {
                taken = take(index);
                break;
            }
        lock_.unlock();
        return taken;
    }

    [[nodiscard]] bool contains(ThreadRef ref) const
    {
        Guard guard{lock_};
        return find(ref) != Capacity;
    }

    // A snapshot; exact only while the owner serializes queue mutation.
    [[nodiscard]] usize size() const { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool empty() const { return size() == 0; }

  private:
    class Guard final
    {
      public:
        explicit Guard(Lock &lock) : lock_(lock) { lock_.lock(); }
        ~Guard() { lock_.unlock(); }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

      private:
        Lock &lock_;
    };

    [[nodiscard]] usize slot(usize index) const { return (head_ + index) % Capacity; }

    [[nodiscard]] usize find(ThreadRef ref) const
    {
        const auto count = count_.load(std::memory_order_relaxed);
        for (usize index = 0; index < count; ++index)
            if (entries_[slot(index)] == ref)
                return index;
        return Capacity;
    }

    // Caller holds the lock and passes an occupied index.
    ThreadRef take(usize index)
    {
        const auto count = count_.load(std::memory_order_relaxed);
        const auto taken = entries_[slot(index)];
        if (index == 0)
        {
            entries_[head_] = {};
            head_ = slot(1);
        }
        else
        {
            for (auto next = index + 1; next < count; ++next)
                entries_[slot(next - 1)] = entries_[slot(next)];
            entries_[slot(count - 1)] = {};
        }
        count_.store(count - 1, std::memory_order_relaxed);
        return taken;
    }

    mutable Lock lock_{};
    usize head_{};
    std::atomic<usize> count_{0};
    std::array<ThreadRef, Capacity> entries_{};
};

} // namespace ok::sched
