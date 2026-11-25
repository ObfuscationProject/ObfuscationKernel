// Model checks of the fixed task storage (include/ok/sched/task_table.hpp):
// slot lifecycle, pins against reap, ranges over [live, zombie] slots and
// over live/recorded TCB slots. The race target (OK_HOSTED_LOCK_MODEL with
// OK_LOCK_DEBUG) runs pins against reap and reallocation on host threads with
// the real task lock. Model coverage only: no guest, IPI or hardware SMP.
#include "ok/sched/task_table.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#if defined(OK_HOSTED_LOCK_MODEL)
#include "hosted_race.hpp"
#endif

using namespace ok;
using namespace ok::sched;

namespace
{
std::atomic<usize> checks{0};
void check(bool condition, const char *what)
{
    checks.fetch_add(1, std::memory_order_relaxed);
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::abort();
    }
}

// A PCB stand-in: the identity the slot was allocated for, written only while
// the slot is free (allocation) and read by pin holders.
struct FakePcb
{
    u64 id{};
    std::array<u64, 4> payload{};
};
using Slot = BasicProcessSlot<FakePcb>;
using Pin = BasicProcessPin<Slot>;
using Range = BasicProcessRange<Slot, FakePcb>;
using ConstRange = BasicProcessRange<const Slot, const FakePcb>;

// Allocation and reap as the scheduler does them, under the slot's task lock.
bool allocate(Slot &slot, u64 id)
{
    TaskGuard guard{slot};
    if (slot.state() != ProcessLifecycle::free || slot.pins != 0) return false;
    slot.pcb.id = id;
    slot.pcb.payload.fill(id);
    slot.pid.store(static_cast<u32>(id), std::memory_order_release);
    slot.lifecycle.store(static_cast<u32>(ProcessLifecycle::live), std::memory_order_release);
    return true;
}
void set_state(Slot &slot, ProcessLifecycle lifecycle)
{
    TaskGuard guard{slot};
    slot.lifecycle.store(static_cast<u32>(lifecycle), std::memory_order_release);
}
void reap(Slot &slot)
{
    TaskGuard guard{slot};
    slot.lifecycle.store(static_cast<u32>(ProcessLifecycle::reaped), std::memory_order_release);
    if (slot.pins == 0) Pin::free_locked(slot);
}

void lifecycle_and_pins()
{
    static Slot slot{};
    check(!Pin::acquire(slot, 0) && !Pin::acquire(slot, 7), "a free slot cannot be pinned");
    check(allocate(slot, 7) && !allocate(slot, 8), "allocation takes a free slot only");
    {
        auto pin = Pin::acquire(slot, 7);
        check(pin && pin.get() == &slot.pcb && slot.pins == 1, "a live slot pins");
        check(!Pin::acquire(slot, 8), "a pin names the slot's own pid");
        auto moved = std::move(pin);
        check(!pin && moved && slot.pins == 1, "a pin moves without a second count");
        set_state(slot, ProcessLifecycle::zombie);
        auto zombie = Pin::acquire(slot, 7);
        check(zombie && slot.pins == 2, "a zombie pins");
        reap(slot);
        check(slot.state() == ProcessLifecycle::reaped && slot.pid.load() == 7, "a pinned reap keeps the slot");
        check(!slot.names(7) && !Pin::acquire(slot, 7), "a reaped slot is invisible and unpinnable");
        check(!allocate(slot, 9), "a reaped slot is not reallocated while pinned");
        zombie.release();
        check(slot.pins == 1 && slot.state() == ProcessLifecycle::reaped, "an earlier unpin keeps it reaped");
        check(moved->id == 7, "the pinned PCB is unchanged");
    }
    check(slot.pins == 0 && slot.state() == ProcessLifecycle::free && slot.pid.load() == 0, "the last unpin frees it");
    check(allocate(slot, 9) && slot.pcb.id == 9, "a freed slot is reallocated");
    reap(slot);
    check(slot.state() == ProcessLifecycle::free, "an unpinned reap frees at once");
    for (auto state : {ProcessLifecycle::embryo, ProcessLifecycle::reaped, ProcessLifecycle::free})
        check(!lifecycle_visible(state), "embryo, reaped and free are invisible");
    for (auto state : {ProcessLifecycle::live, ProcessLifecycle::exiting, ProcessLifecycle::zombie})
        check(lifecycle_visible(state), "live, exiting and zombie are visible");
}

void ranges()
{
    static std::array<Slot, max_processes> slots{};
    check(allocate(slots[3], 30) && allocate(slots[5], 50) && allocate(slots[9], 90), "three slots");
    set_state(slots[5], ProcessLifecycle::zombie);
    check(allocate(slots[7], 70), "a fourth slot");
    {
        auto pin = Pin::acquire(slots[7], 70);
        reap(slots[7]);
        Range range{slots.data(), slots.size()};
        u64 sum = 0;
        usize count = 0;
        for (auto &pcb : range)
        {
            sum += pcb.id;
            ++count;
        }
        check(count == 3 && sum == 170 && range.size() == 3, "the range yields [live, zombie] in slot order");
        check(range.at(5) == &slots[5].pcb && !range.at(7) && !range.at(0), "at() sees visible slots only");
        const ConstRange view = range;
        check(view.size() == 3 && view.at(9) == &slots[9].pcb, "a mutable range reads as a const one");
    }
    check(slots[7].state() == ProcessLifecycle::free, "the reaped slot frees on unpin");

    struct Tcb
    {
        u64 tid{};
        ThreadSlotState slot_state{ThreadSlotState::free};
    };
    std::array<Tcb, max_threads_per_process> tcbs{};
    tcbs[0] = {10, ThreadSlotState::exited};
    tcbs[1] = {11, ThreadSlotState::live};
    tcbs[2] = {12, ThreadSlotState::embryo};
    tcbs[4] = {14, ThreadSlotState::live};
    tcbs[5] = {15, ThreadSlotState::exited};
    ThreadSlotRange<Tcb, LiveThreadSlot> live{tcbs.data(), tcbs.size()};
    ThreadSlotRange<Tcb, RecordedThreadSlot> records{tcbs.data(), tcbs.size()};
    u64 live_sum = 0, record_sum = 0;
    for (auto &tcb : live) live_sum += tcb.tid;
    for (auto &tcb : records) record_sum += tcb.tid;
    check(live.size() == 2 && live_sum == 25, "live slots skip exited, embryo and free");
    check(records.size() == 4 && record_sum == 50, "records hold live and exited slots");
    static_assert(thread_index(63, 7) == 511 && max_thread_slots == 512);
    constexpr ThreadHandle none{};
    static_assert(!none.valid() && ThreadHandle{5, 9}.valid() && !ThreadHandle{5, 0}.valid());
}

#if defined(OK_HOSTED_LOCK_MODEL)
// CPU 1 reaps and reallocates four slots without pause; CPUs 2-4 pin whatever
// pid a slot names and check that the PCB stays the one they pinned for the
// whole hold. A pinned slot must never be reallocated under its holder.
void pins_race_reap()
{
    static std::array<Slot, 4> slots{};
    static std::atomic<bool> stop{false};
    static std::atomic<u64> pins_taken{0}, torn{0}, reallocations{0};
    for (usize i = 0; i < slots.size(); ++i) check(allocate(slots[i], 1 + i), "race setup");
    test::race(4, [](unsigned cpu) {
        if (cpu == 1)
        {
            u64 next = 100;
            for (u32 round = 0; round < 200000; ++round)
            {
                auto &slot = slots[round % slots.size()];
                if (slot.state() == ProcessLifecycle::live) reap(slot);
                if (allocate(slot, next)) { ++next; reallocations.fetch_add(1, std::memory_order_relaxed); }
            }
            stop.store(true, std::memory_order_release);
            return;
        }
        test::Sequence sequence{cpu};
        while (!stop.load(std::memory_order_acquire))
        {
            auto &slot = slots[sequence.next() % slots.size()];
            const u64 id = slot.pid.load(std::memory_order_acquire);
            auto pin = Pin::acquire(slot, id);
            if (!pin) continue;
            pins_taken.fetch_add(1, std::memory_order_relaxed);
            for (int read = 0; read < 8; ++read)
                if (pin->id != id || pin->payload[read % 4] != id) torn.fetch_add(1, std::memory_order_relaxed);
        }
    });
    u32 pins_left = 0;
    for (auto &slot : slots) pins_left += slot.pins;
    check(torn.load() == 0, "no pinned PCB is reallocated under its holder");
    check(pins_left == 0, "every pin is dropped");
    check(pins_taken.load() > 0 && reallocations.load() > 0, "the race pinned and reallocated");
    check(test::lock_violations() == 0, "the task lock is taken in order");
    std::printf("task-table race pins=%llu reallocations=%llu torn=%llu\n",
                static_cast<unsigned long long>(pins_taken.load()),
                static_cast<unsigned long long>(reallocations.load()), static_cast<unsigned long long>(torn.load()));
}
#endif
} // namespace

int main()
{
    lifecycle_and_pins();
    ranges();
#if defined(OK_HOSTED_LOCK_MODEL)
    pins_race_reap();
    std::printf("PASS task-table race checks=%zu scope=hosted-model threads=4 guest=0\n", checks.load());
#else
    std::printf("PASS task-table checks=%zu scope=hosted-model guest=0 smp=0\n", checks.load());
#endif
}
