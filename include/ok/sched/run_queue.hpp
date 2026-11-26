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

// A thread named by its identities: the external form (syscalls, tests).
struct ThreadRef
{
    ProcessId pid{};
    ThreadId tid{};

    [[nodiscard]] constexpr bool operator==(const ThreadRef &) const = default;
};

inline constexpr u16 invalid_thread_index = 0xffff;

// A thread named by its fixed TCB slot (process slot * 8 + thread slot) and
// its id. Slots never move and tids are never reused (they stay below 2^30),
// so a handle names its thread exactly while that slot still holds that tid.
// Placement records carry handles, never PCB/TCB addresses.
struct ThreadHandle
{
    u16 index{invalid_thread_index};
    u32 tid{};

    [[nodiscard]] constexpr bool valid() const { return index != invalid_thread_index && tid != 0; }
    [[nodiscard]] constexpr bool operator==(const ThreadHandle &) const = default;
};

template <typename T>
concept RunQueueLock = requires(T &lock) {
    lock.lock();
    lock.unlock();
    { lock.try_lock() } -> std::same_as<bool>;
};

struct AnyThreadRef
{
    template <typename Entry> [[nodiscard]] constexpr bool operator()(Entry) const { return true; }
};

// Every operation takes only this queue's lock and releases it before
// returning, so a caller never holds two queue locks. `try_take_first` is the
// stealing edge: it never spins on a contended victim.
template <typename Q>
concept RunQueue = requires(Q &queue, const Q &view, typename Q::Entry ref, AnyThreadRef any) {
    { queue.push(ref) } -> std::same_as<Status>;
    { queue.pop() } -> std::same_as<std::optional<typename Q::Entry>>;
    { queue.remove(ref) } -> std::same_as<bool>;
    { queue.try_take_first(any) } -> std::same_as<std::optional<typename Q::Entry>>;
    { view.contains(ref) } -> std::same_as<bool>;
    { view.size() } -> std::same_as<usize>;
};

// Bounded FIFO of thread names (identities or handles) owned by one CPU. A
// closed queue (its CPU is offline) refuses pushes; `size()` is an atomic
// mirror of the entry count that idle checks and victim choice load without
// the lock.
template <usize Capacity, RunQueueLock Lock = smp::SpinLock, typename EntryType = ThreadRef> class CpuRunQueue final
{
    static_assert(Capacity != 0);

  public:
    using Entry = EntryType;
    constexpr CpuRunQueue() = default;
    CpuRunQueue(const CpuRunQueue &) = delete;
    CpuRunQueue &operator=(const CpuRunQueue &) = delete;

    [[nodiscard]] static constexpr usize capacity() { return Capacity; }

    Status push(Entry ref)
    {
        Guard guard{lock_};
        if (closed_.load(std::memory_order_relaxed))
            return Status::denied("CPU run queue is closed");
        const auto count = count_.load(std::memory_order_relaxed);
        if (count == Capacity)
            return Status::overflow("CPU run queue is full");
        entries_[slot(count)] = ref;
        count_.store(count + 1, std::memory_order_relaxed);
        return {};
    }

    std::optional<Entry> pop()
    {
        Guard guard{lock_};
        if (count_.load(std::memory_order_relaxed) == 0)
            return std::nullopt;
        return take(0);
    }

    // O(n); the remaining entries keep their FIFO order.
    bool remove(Entry ref)
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
        requires std::predicate<Predicate &, Entry>
    std::optional<Entry> try_take_first(Predicate &&eligible)
    {
        if (!lock_.try_lock())
            return std::nullopt;
        std::optional<Entry> taken{};
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

    [[nodiscard]] bool contains(Entry ref) const
    {
        Guard guard{lock_};
        return find(ref) != Capacity;
    }

    // A snapshot; exact only while the owner serializes queue mutation.
    [[nodiscard]] usize size() const { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool empty() const { return size() == 0; }

    // Closing does not drain: the caller pops the remaining entries and
    // places them again. A push that raced the close was refused.
    void set_closed(bool closed)
    {
        Guard guard{lock_};
        closed_.store(closed, std::memory_order_relaxed);
    }
    [[nodiscard]] bool closed() const { return closed_.load(std::memory_order_relaxed); }

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

    [[nodiscard]] usize find(Entry ref) const
    {
        const auto count = count_.load(std::memory_order_relaxed);
        for (usize index = 0; index < count; ++index)
            if (entries_[slot(index)] == ref)
                return index;
        return Capacity;
    }

    // Caller holds the lock and passes an occupied index.
    Entry take(usize index)
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
    std::atomic<bool> closed_{false};
    std::array<Entry, Capacity> entries_{};
};

} // namespace ok::sched
