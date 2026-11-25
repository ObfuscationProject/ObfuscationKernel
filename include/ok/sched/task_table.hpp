#pragma once

// Fixed task storage (smp-scaling WP2, spec section 3). Process slots and the
// TCB slots inside them never move or compact, so a slot index plus a task id
// names a task for ever: ids are never reused. Readers that hold no lock see
// the slot's pid and lifecycle words; everything else in a slot is reached
// through a pin, through the caller's own CPU slot, or under the task table.

#include "ok/core/types.hpp"
#include "ok/sched/run_queue.hpp"
#include "ok/smp/lock.hpp"

#include <array>
#include <atomic>
#include <concepts>

namespace ok::sched
{

inline constexpr usize max_processes = 64;
inline constexpr usize max_threads_per_process = 8;
inline constexpr usize max_thread_slots = max_processes * max_threads_per_process;
static_assert(max_thread_slots < invalid_thread_index);

// A TCB slot: free (no record), embryo (being prepared by clone), live (a
// thread that has not exited) or exited (an exited thread's record, kept until
// the slot is reused). Only live threads run, wait or receive signals.
enum class ThreadSlotState : u8
{
    free,
    embryo,
    live,
    exited,
};

// A process slot's lifecycle. Lookups and ranges see [live, zombie]; a reaped
// slot that is still pinned stays reserved until its last pin goes.
enum class ProcessLifecycle : u32
{
    free,
    embryo,
    live,
    exiting,
    zombie,
    reaped,
};

[[nodiscard]] constexpr bool lifecycle_visible(ProcessLifecycle lifecycle)
{
    return lifecycle == ProcessLifecycle::live || lifecycle == ProcessLifecycle::exiting ||
           lifecycle == ProcessLifecycle::zombie;
}

[[nodiscard]] constexpr u16 thread_index(usize process_slot, usize thread_slot)
{
    return static_cast<u16>(process_slot * max_threads_per_process + thread_slot);
}

// One process slot. `pid` (0 when free) and `lifecycle` are stored under the
// slot's task lock (allocation also holds the task table) and loaded by
// anyone; `pins` is read and written only under the task lock. Per-thread
// words other CPUs may read without the PCB live here as well.
template <typename Pcb> struct BasicProcessSlot
{
    smp::FineLock<smp::LockRank::task> task{};
    std::atomic<u32> pid{0};
    std::atomic<u32> lifecycle{static_cast<u32>(ProcessLifecycle::free)};
    // sched_setaffinity's per-thread masks (docs/modules/sysfs.md), by TCB
    // slot; the effective mask is the process mask & this one.
    std::array<std::atomic<u32>, max_threads_per_process> thread_affinity{};
    u32 pins{};
    Pcb pcb{};

    [[nodiscard]] ProcessLifecycle state() const
    {
        return static_cast<ProcessLifecycle>(lifecycle.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool visible() const { return lifecycle_visible(state()); }
    // Whether the slot names `id` and is visible; a lock-free first test that
    // a pin repeats under the task lock.
    [[nodiscard]] bool names(u64 id) const
    {
        return id != 0 && pid.load(std::memory_order_acquire) == id && visible();
    }
};

// Holds a slot's task lock for a scope (the run queue Guard shape).
template <typename Slot> class TaskGuard final
{
  public:
    explicit TaskGuard(Slot &slot) : slot_(slot) { slot_.task.lock(); }
    ~TaskGuard() { slot_.task.unlock(); }
    TaskGuard(const TaskGuard &) = delete;
    TaskGuard &operator=(const TaskGuard &) = delete;

  private:
    Slot &slot_;
};

// Keeps a process slot from being freed (spec 3.2): while a pin is held the
// slot keeps its pid and its PCB, even after the process was reaped. Taken
// while holding no lock at or above `task`. The last unpin of a reaped slot
// frees it.
template <typename Slot> class BasicProcessPin final
{
  public:
    BasicProcessPin() = default;
    // Pins the slot when it still names `id` and is in [live, zombie].
    [[nodiscard]] static BasicProcessPin acquire(Slot &slot, u64 id)
    {
        if (!slot.names(id)) return {};
        TaskGuard guard{slot};
        if (!slot.names(id) || slot.pins == ~u32{0}) return {};
        ++slot.pins;
        return BasicProcessPin{slot};
    }
    BasicProcessPin(BasicProcessPin &&other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }
    BasicProcessPin &operator=(BasicProcessPin &&other) noexcept
    {
        if (this != &other)
        {
            release();
            slot_ = other.slot_;
            other.slot_ = nullptr;
        }
        return *this;
    }
    BasicProcessPin(const BasicProcessPin &) = delete;
    BasicProcessPin &operator=(const BasicProcessPin &) = delete;
    ~BasicProcessPin() { release(); }

    [[nodiscard]] explicit operator bool() const { return slot_ != nullptr; }
    [[nodiscard]] Slot *slot() const { return slot_; }
    [[nodiscard]] auto *get() const { return slot_ ? &slot_->pcb : nullptr; }
    [[nodiscard]] auto *operator->() const { return get(); }

    void release()
    {
        if (!slot_) return;
        unpin(*slot_);
        slot_ = nullptr;
    }

    // Drops one pin; the last pin of a reaped slot frees it.
    static void unpin(Slot &slot)
    {
        TaskGuard guard{slot};
        if (slot.pins == 0)
        {
            smp::count_check(smp::KernelCheck::pin, "ProcessPin underflow");
            return;
        }
        if (--slot.pins == 0 && slot.state() == ProcessLifecycle::reaped) free_locked(slot);
    }
    // Caller holds the slot's task lock and no pin remains.
    static void free_locked(Slot &slot)
    {
        slot.lifecycle.store(static_cast<u32>(ProcessLifecycle::free), std::memory_order_release);
        slot.pid.store(0, std::memory_order_release);
    }

  private:
    explicit BasicProcessPin(Slot &slot) : slot_(&slot) {}
    Slot *slot_{};
};

// The [live, zombie] processes of a slot table, in slot order. Iteration loads
// each slot's lifecycle; callers hold the kernel lock (later the task table).
template <typename Slot, typename Pcb> class BasicProcessRange final
{
  public:
    constexpr BasicProcessRange(Slot *slots, usize count) : slots_(slots), count_(count) {}
    // A mutable range reads as a const one.
    template <typename OtherSlot, typename OtherPcb>
        requires(!std::same_as<OtherSlot, Slot> && std::convertible_to<OtherSlot *, Slot *>)
    constexpr BasicProcessRange(const BasicProcessRange<OtherSlot, OtherPcb> &other)
        : slots_(other.slot_data()), count_(other.slot_count())
    {
    }
    [[nodiscard]] Slot *slot_data() const { return slots_; }
    [[nodiscard]] usize slot_count() const { return count_; }

    class Iterator
    {
      public:
        using value_type = Pcb;
        using difference_type = std::ptrdiff_t;
        Iterator() = default;
        Iterator(Slot *slot, Slot *end) : slot_(slot), end_(end) { skip(); }
        Pcb &operator*() const { return slot_->pcb; }
        Pcb *operator->() const { return &slot_->pcb; }
        Iterator &operator++()
        {
            ++slot_;
            skip();
            return *this;
        }
        Iterator operator++(int)
        {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const Iterator &other) const { return slot_ == other.slot_; }

      private:
        void skip()
        {
            while (slot_ != end_ && !slot_->visible()) ++slot_;
        }
        Slot *slot_{};
        Slot *end_{};
    };

    [[nodiscard]] Iterator begin() const { return {slots_, slots_ + count_}; }
    [[nodiscard]] Iterator end() const { return {slots_ + count_, slots_ + count_}; }
    // Slot positions for policies that resume after a given process.
    [[nodiscard]] static constexpr usize capacity() { return max_processes; }
    [[nodiscard]] Pcb *at(usize slot) const
    {
        return slot < count_ && slots_[slot].visible() ? &slots_[slot].pcb : nullptr;
    }
    [[nodiscard]] usize size() const
    {
        usize visible = 0;
        for (usize i = 0; i < count_; ++i) visible += slots_[i].visible();
        return visible;
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

  private:
    Slot *slots_;
    usize count_;
};

// A range over a fixed TCB array that yields the slots whose state passes
// `Filter` (live threads, or every record), in slot order.
template <typename Tcb, typename Filter> class ThreadSlotRange final
{
  public:
    constexpr ThreadSlotRange(Tcb *slots, usize count) : slots_(slots), count_(count) {}

    class Iterator
    {
      public:
        using value_type = Tcb;
        using difference_type = std::ptrdiff_t;
        Iterator() = default;
        Iterator(Tcb *slot, Tcb *end) : slot_(slot), end_(end) { skip(); }
        Tcb &operator*() const { return *slot_; }
        Tcb *operator->() const { return slot_; }
        Iterator &operator++()
        {
            ++slot_;
            skip();
            return *this;
        }
        Iterator operator++(int)
        {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const Iterator &other) const { return slot_ == other.slot_; }

      private:
        void skip()
        {
            while (slot_ != end_ && !Filter{}(slot_->slot_state)) ++slot_;
        }
        Tcb *slot_{};
        Tcb *end_{};
    };

    [[nodiscard]] Iterator begin() const { return {slots_, slots_ + count_}; }
    [[nodiscard]] Iterator end() const { return {slots_ + count_, slots_ + count_}; }
    [[nodiscard]] usize size() const
    {
        usize count = 0;
        for (usize i = 0; i < count_; ++i) count += Filter{}(slots_[i].slot_state);
        return count;
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

  private:
    Tcb *slots_;
    usize count_;
};

struct LiveThreadSlot
{
    [[nodiscard]] constexpr bool operator()(ThreadSlotState state) const { return state == ThreadSlotState::live; }
};
struct RecordedThreadSlot
{
    [[nodiscard]] constexpr bool operator()(ThreadSlotState state) const
    {
        return state == ThreadSlotState::live || state == ThreadSlotState::exited;
    }
};

// What a CPU runs, written only by that CPU (and by configure/teardown under
// the kernel lock). The PCB/TCB pointers are the CPU's caller binding (spec
// 3.5): valid while the thread occupies the CPU, because its slot is never
// freed while it does.
template <typename Pcb, typename Tcb> struct BasicCpuSlot
{
    u64 pid{};
    u64 tid{};
    Pcb *process{};
    Tcb *thread{};
    u64 dispatches{};
    u64 busy_dispatches{};

    [[nodiscard]] bool occupied() const { return tid != 0; }
};

} // namespace ok::sched
