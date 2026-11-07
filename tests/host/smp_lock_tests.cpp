// Hosted model test for ranked spin locks (feat/smp-bringup). Threads stand in
// for CPUs through ScopedHostedCpu; no CPU, interrupt mask or IPI executes.
// Built twice: with OK_LOCK_DEBUG (the full suite) and without (release).
#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_lock.hpp"
#include "ok/smp/lock.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace ok;

namespace
{

int checks = 0;

void expect(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

using SchedulerLock = smp::RankedSpinLock<smp::LockRank::scheduler>;
using RunQueueLock = smp::RankedSpinLock<smp::LockRank::run_queue>;
using WaitQueueLock = smp::RankedSpinLock<smp::LockRank::wait_queue>;
using ConsoleLock = smp::RankedSpinLock<smp::LockRank::console>;

// Constant initialization is the kernel's only option for globals.
constinit SchedulerLock global_scheduler_lock;
constinit RunQueueLock global_run_queue_lock;

constexpr int contention_threads = 4;
constexpr int contention_rounds = 20000;

// Mutual exclusion under contention: four "CPUs" increment a plain counter.
void check_contention()
{
    long counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < contention_threads; ++cpu)
    {
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            for (int round = 0; round < contention_rounds; ++round)
            {
                if (round % 2 == 0)
                {
                    SchedulerLock::Guard guard(global_scheduler_lock);
                    if (inside.fetch_add(1, std::memory_order_relaxed) != 0)
                        overlap.store(true);
                    ++counter;
                    inside.fetch_sub(1, std::memory_order_relaxed);
                }
                else
                {
                    smp::RankedSpinGuard guard{global_scheduler_lock};
                    smp::RankedSpinGuard nested{global_run_queue_lock};
                    if (inside.fetch_add(1, std::memory_order_relaxed) != 0)
                        overlap.store(true);
                    ++counter;
                    inside.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
    expect(counter == long{contention_threads} * contention_rounds, "contended counter lost increments");
    expect(!overlap.load(), "two CPUs were inside the critical section");
    expect(!global_scheduler_lock.owned_by_current_cpu() && global_scheduler_lock.try_lock(),
           "contended lock was left held");
    global_scheduler_lock.unlock();
}

// Fake interrupt mask: enabled until saved, restored only if it was enabled.
struct FakeInterrupts
{
    static inline bool enabled = true;
    static inline int saves = 0;
    static inline int restores = 0;

    [[nodiscard]] static uptr save_and_disable_interrupts() noexcept
    {
        ++saves;
        const uptr previous = enabled ? 1 : 0;
        enabled = false;
        return previous;
    }
    static void restore_interrupts(uptr saved) noexcept
    {
        ++restores;
        if (saved != 0)
            enabled = true;
    }
};
static_assert(smp::InterruptMask<FakeInterrupts>);

void check_guards()
{
    SchedulerLock outer;
    RunQueueLock inner;
    {
        smp::RankedSpinGuard<smp::LockRank::scheduler, FakeInterrupts> guard{outer};
        expect(!FakeInterrupts::enabled && outer.owned_by_current_cpu(), "guard did not mask before holding");
        {
            smp::RankedSpinGuard<smp::LockRank::run_queue, FakeInterrupts> nested{inner};
            expect(!FakeInterrupts::enabled && inner.owned_by_current_cpu(), "nested guard did not hold masked");
        }
        expect(!FakeInterrupts::enabled, "nested guard re-enabled interrupts the outer guard masked");
        expect(!inner.owned_by_current_cpu(), "nested guard did not release");
    }
    expect(FakeInterrupts::enabled && !outer.owned_by_current_cpu(), "guard did not restore enabled interrupts");
    FakeInterrupts::enabled = false;
    {
        smp::RankedSpinGuard<smp::LockRank::scheduler, FakeInterrupts> guard{outer};
    }
    expect(!FakeInterrupts::enabled, "guard enabled interrupts that were masked on entry");
    expect(FakeInterrupts::saves == 3 && FakeInterrupts::restores == 3, "guard save/restore count mismatch");
    {
        RunQueueLock::Guard guard(inner);
        expect(inner.owned_by_current_cpu(), "plain guard did not hold");
    }
    expect(FakeInterrupts::saves == 3 && !inner.owned_by_current_cpu(), "plain guard touched interrupts or held");

    // Hosted builds of the arch fallbacks never mask anything.
    expect(arch::ArchOperations::save_and_disable_interrupts() == 0, "hosted save_and_disable is not inert");
    arch::ArchOperations::restore_interrupts(1);
    arch::ArchOperations::spin_pause();
    expect(arch::ArchOperations::cpu_index() == 0, "hosted cpu_index is not the boot CPU");
    expect(smp::KernelInterruptMask::save_and_disable_interrupts() == 0, "hosted kernel mask is not inert");
}

void check_hosted_cpu()
{
    expect(smp::current_cpu() == 0, "hosted CPU does not default to 0");
    {
        const smp::ScopedHostedCpu outer(3);
        expect(smp::current_cpu() == 3, "scoped hosted CPU not applied");
        {
            const smp::ScopedHostedCpu inner(1);
            expect(smp::current_cpu() == 1, "nested hosted CPU not applied");
        }
        expect(smp::current_cpu() == 3, "nested hosted CPU not restored");
    }
    expect(smp::current_cpu() == 0, "hosted CPU not restored");
    // A lock is owned per CPU, not per thread.
    SchedulerLock lock;
    {
        const smp::ScopedHostedCpu scoped(2);
        lock.lock();
        expect(lock.owned_by_current_cpu(), "owner CPU not recorded");
    }
    expect(!lock.owned_by_current_cpu(), "another CPU's hold reported as ours");
    {
        const smp::ScopedHostedCpu scoped(2);
        lock.unlock();
        expect(!lock.owned_by_current_cpu(), "unlock left the owner");
    }
    // spin_pause in a waiter: CPU 1 waits while CPU 0 holds.
    SchedulerLock contended;
    contended.lock();
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        const smp::ScopedHostedCpu scoped(1);
        contended.lock();
        acquired.store(true);
        contended.unlock();
    });
    for (int spin = 0; spin < 1000; ++spin)
        std::this_thread::yield();
    expect(!acquired.load(), "waiter acquired a held lock");
    contended.unlock();
    waiter.join();
    expect(acquired.load(), "waiter never acquired the released lock");
}

// The big kernel lock helpers keep this CPU's mode bits and accounting: an
// acquisition, a contended one (the waiter spins with SPINNING set) and a
// completed stretch per release of a stretch marked as work.
void check_bkl_accounting()
{
    auto &self = smp::bkl_cpus[0];
    const auto acquired = self.acquired.load(), contended = self.contended.load(), done = self.done.load();
    // Without the lock the mark does nothing; an unmarked stretch (an idle
    // scan) is an acquisition but no completed work.
    smp::bkl_mark_work();
    expect(smp::cpu_mode(0) == 0, "bkl_mark_work marked a CPU without the kernel lock");
    smp::bkl_lock();
    smp::bkl_unlock();
    expect(self.acquired.load() == acquired + 1 && self.done.load() == done && smp::cpu_mode(0) == 0,
           "an unmarked stretch counted as work");
    smp::bkl_lock();
    expect(smp::kernel_lock.owned_by_current_cpu() && (smp::cpu_mode(0) & smp::cpu_mode_bkl_held) != 0,
           "bkl_lock did not hold or mark BKL_HELD");
    smp::bkl_mark_work();
    expect((smp::cpu_mode(0) & smp::cpu_mode_kernel_work) != 0, "bkl_mark_work did not mark the stretch");
    std::atomic<bool> trying{false}, acquired_by_one{false};
    std::thread waiter([&] {
        const smp::ScopedHostedCpu scoped(1);
        expect(!smp::bkl_try_lock(), "bkl_try_lock took a held lock");
        trying.store(true);
        smp::bkl_lock();
        smp::bkl_mark_work();
        acquired_by_one.store(true);
        smp::bkl_unlock();
    });
    while (!trying.load())
        std::this_thread::yield();
    bool spinning_seen = false;
    for (int spin = 0; spin < 1000000 && !spinning_seen; ++spin)
    {
        spinning_seen = (smp::cpu_mode(1) & smp::cpu_mode_spinning) != 0;
        std::this_thread::yield();
    }
    expect(spinning_seen && !acquired_by_one.load(), "a CPU waiting for the kernel lock was not marked SPINNING");
    expect(self.contended.load() == contended, "the holder counted contention");
    smp::bkl_unlock();
    waiter.join();
    expect(acquired_by_one.load() && smp::cpu_mode(1) == 0 && smp::cpu_mode(0) == 0,
           "mode bits left behind after release");
    expect(self.acquired.load() == acquired + 2 && self.done.load() == done + 1, "holder accounting");
    const auto &other = smp::bkl_cpus[1];
    expect(other.acquired.load() >= 1 && other.contended.load() >= 1 && other.done.load() >= 1,
           "waiter accounting");
    {
        smp::BklGuard guard;
        expect(smp::kernel_lock.owned_by_current_cpu(), "BklGuard did not hold");
        smp::bkl_mark_work();
    }
    expect(!smp::kernel_lock.owned_by_current_cpu() && self.acquired.load() == acquired + 3 &&
               self.done.load() == done + 2 && smp::cpu_mode(0) == 0,
           "BklGuard accounting");
    expect(smp::bkl_try_lock() && self.contended.load() == contended, "uncontended bkl_try_lock");
    smp::bkl_unlock();
    {
        const smp::SpinningScope outer;
        {
            const smp::SpinningScope inner;
        }
        expect((smp::cpu_mode(0) & smp::cpu_mode_spinning) != 0, "a nested SpinningScope cleared the outer bit");
    }
    expect(smp::cpu_mode(0) == 0, "SpinningScope left SPINNING set");
}

// A gate that is not a RankedSpinLock: an owner word (CPU + 1) under an
// unranked lock, a busy result on reentry from the owning CPU, and a wait
// outside that lock with the CPU marked SPINNING. It records its hold at the
// fs_provider rank through the public lock-debug helpers.
class ModelGate
{
  public:
    // False: this CPU already owns the gate (reentry); nothing is taken.
    bool enter()
    {
        const auto self = static_cast<u32>(smp::current_cpu()) + 1;
        {
            // Only this CPU sets or clears its own ownership.
            smp::ScopedSpinLock guard(lock_);
            if (owner_ == self)
                return false;
        }
        smp::lock_debug_may_wait(this, smp::LockRank::fs_provider);
        for (bool counted = false;;)
        {
            {
                smp::ScopedSpinLock guard(lock_);
                if (owner_ == self)
                    return false;
                if (owner_ == 0)
                {
                    owner_ = self;
                    break;
                }
            }
            if (!counted)
            {
                counted = true;
                waits.fetch_add(1);
            }
            const smp::SpinningScope spinning;
            while (busy())
                arch::ArchOperations::spin_pause();
        }
        smp::lock_debug_hold(this, smp::LockRank::fs_provider);
        return true;
    }
    void leave()
    {
        smp::lock_debug_release(this, smp::LockRank::fs_provider);
        smp::ScopedSpinLock guard(lock_);
        owner_ = 0;
    }
    bool busy()
    {
        smp::ScopedSpinLock guard(lock_);
        return owner_ != 0;
    }
    std::atomic<int> waits{0};

  private:
    smp::SpinLock lock_;
    u32 owner_{0};
};

// Two hosted CPUs through the gate: one owner at a time, reentry refused on
// the owning CPU only, every increment kept, and a CPU that waited was seen
// SPINNING by the other.
void check_gate_two_cpus()
{
    ModelGate gate;
    long counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false}, reentry{false};
    constexpr int rounds = 20000;
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < 2; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(4 + cpu));
            for (int round = 0; round < rounds; ++round)
            {
                if (!gate.enter())
                    reentry.store(true);
                if (inside.fetch_add(1) != 0)
                    overlap.store(true);
                if (gate.enter())
                    reentry.store(true); // Reentry must be refused.
                ++counter;
                inside.fetch_sub(1);
                gate.leave();
            }
        });
    for (auto &thread : threads)
        thread.join();
    expect(!overlap.load() && counter == 2L * rounds, "the gate let two CPUs in");
    expect(!reentry.load(), "the gate refused a free entry or admitted reentry");
    expect(!gate.busy() && smp::cpu_mode(4) == 0 && smp::cpu_mode(5) == 0, "the gate or a mode bit was left held");
    expect(smp::lock_debug_depth(4) == 0 && smp::lock_debug_depth(5) == 0, "the gate left a recorded hold");
    // Deterministic wait: CPU 6 owns the gate while CPU 7 asks for it.
    const auto waits = gate.waits.load();
    std::atomic<bool> held{false}, release{false}, entered{false};
    std::thread owner([&] {
        const smp::ScopedHostedCpu scoped(6);
        expect(gate.enter(), "owner entry");
        held.store(true);
        while (!release.load())
            std::this_thread::yield();
        gate.leave();
    });
    while (!held.load())
        std::this_thread::yield();
    std::thread waiter([&] {
        const smp::ScopedHostedCpu scoped(7);
        expect(gate.enter(), "waiter entry");
        entered.store(true);
        gate.leave();
    });
    bool seen = false;
    for (int spin = 0; spin < 1000000 && !seen; ++spin)
    {
        seen = (smp::cpu_mode(7) & smp::cpu_mode_spinning) != 0;
        std::this_thread::yield();
    }
    expect(seen && !entered.load(), "a CPU waiting for the gate was not marked SPINNING or got in");
    release.store(true);
    owner.join();
    waiter.join();
    expect(entered.load() && gate.waits.load() == waits + 1 && smp::cpu_mode(7) == 0, "gate waiter accounting");
}

#if defined(OK_LOCK_DEBUG)

struct Reports
{
    int count{0};
    smp::LockViolation last{};
};
thread_local Reports reports;

void record_report(const smp::LockViolation &violation) noexcept
{
    ++reports.count;
    reports.last = violation;
}

void reset_reports()
{
    reports = {};
}

bool reported(smp::LockViolationKind kind, smp::LockRank held, smp::LockRank wanted, const void *lock,
              smp::CpuId cpu)
{
    return reports.count == 1 && reports.last.kind == kind && reports.last.held == held &&
           reports.last.wanted == wanted && reports.last.lock == lock && reports.last.cpu == cpu;
}

const smp::LockDebugCpu &record(smp::CpuId cpu)
{
    return smp::lock_debug_cpus[cpu];
}

// Every debug check runs on its own CPU id so per-CPU records do not mix.
template <typename Body> void on_cpu(smp::CpuId cpu, Body body)
{
    std::thread thread([&] {
        const smp::ScopedHostedCpu scoped(cpu);
        reset_reports();
        body();
        expect(record(cpu).depth == 0, "debug record left holds behind");
    });
    thread.join();
}

void check_order_violation()
{
    on_cpu(0, [] {
        SchedulerLock scheduler;
        RunQueueLock run_queue;
        WaitQueueLock wait_queue;
        scheduler.lock();
        run_queue.lock();
        wait_queue.lock();
        expect(reports.count == 0 && record(0).depth == 3, "ordered acquisition reported");
        wait_queue.unlock();
        run_queue.unlock();
        scheduler.unlock();
        expect(reports.count == 0 && record(0).depth == 0, "ordered release reported");

        const auto before = smp::lock_violation_count(0);
        run_queue.lock();
        scheduler.lock(); // Free, so it proceeds after the report.
        expect(reported(smp::LockViolationKind::order, smp::LockRank::run_queue, smp::LockRank::scheduler,
                        &scheduler, 0),
               "reverse order not reported");
        expect(scheduler.owned_by_current_cpu(), "reverse-order lock() did not take a free lock");
        expect(smp::lock_violation_count(0) == before + 1, "violation not counted on its CPU");
        scheduler.unlock();
        run_queue.unlock();
        expect(reports.count == 1, "LIFO release after a reported order reported again");

        // Equal rank is not strictly above: two run queues need a defined order.
        reset_reports();
        RunQueueLock other;
        run_queue.lock();
        other.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::run_queue, smp::LockRank::run_queue, &other,
                        0),
               "equal rank not reported");
        other.unlock();
        run_queue.unlock();

        // The order check compares against every hold, not just the top.
        reset_reports();
        ConsoleLock console;
        scheduler.lock();
        console.lock();
        expect(run_queue.try_lock(), "try_lock under a higher rank failed");
        expect(reported(smp::LockViolationKind::order, smp::LockRank::console, smp::LockRank::run_queue, &run_queue,
                        0),
               "try_lock below a held rank not reported");
        reset_reports();
        WaitQueueLock wait;
        wait.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::console, smp::LockRank::wait_queue, &wait, 0),
               "rank below a non-top hold not reported");
        wait.unlock();
        run_queue.unlock();
        console.unlock();
        scheduler.unlock();
        expect(reports.count == 1, "LIFO release reported");
    });
}

void check_self_deadlock()
{
    on_cpu(1, [] {
        SchedulerLock lock;
        lock.lock();
        // Reported before spinning: the second lock() returns instead of hanging.
        lock.lock();
        expect(reported(smp::LockViolationKind::self_deadlock, smp::LockRank::scheduler, smp::LockRank::scheduler,
                        &lock, 1),
               "self-deadlock not reported");
        expect(lock.owned_by_current_cpu() && record(1).depth == 1, "self-deadlock changed the hold");
        // The refused lock()'s unlock() keeps the outer hold; the outer one releases it.
        lock.unlock();
        expect(lock.owned_by_current_cpu() && record(1).depth == 1 && reports.count == 1,
               "the refused lock()'s unlock released the outer hold");
        lock.unlock();
        expect(!lock.owned_by_current_cpu() && reports.count == 1, "unlock after a self-deadlock report failed");
    });
}

void check_unlock_not_owner()
{
    on_cpu(2, [] {
        SchedulerLock lock;
        lock.unlock(); // Free.
        expect(reported(smp::LockViolationKind::not_owner, smp::LockRank::scheduler, smp::LockRank::scheduler, &lock,
                        2),
               "unlock of a free lock not reported");
        reset_reports();
        lock.lock();
        std::thread foreign([&] {
            const smp::ScopedHostedCpu scoped(3);
            lock.unlock(); // Held by CPU 2.
            expect(reported(smp::LockViolationKind::not_owner, smp::LockRank::scheduler, smp::LockRank::scheduler,
                            &lock, 3),
                   "unlock of another CPU's lock not reported");
            expect(!lock.try_lock(), "foreign unlock released the owner's lock");
        });
        foreign.join();
        expect(lock.owned_by_current_cpu() && record(2).depth == 1 && reports.count == 0, "owner lost its hold");
        lock.unlock();
        expect(reports.count == 0, "owner unlock reported");
    });
}

void check_lifo_violation()
{
    on_cpu(3, [] {
        SchedulerLock scheduler;
        RunQueueLock run_queue;
        WaitQueueLock wait_queue;
        scheduler.lock();
        run_queue.lock();
        wait_queue.lock();
        run_queue.unlock();
        expect(reported(smp::LockViolationKind::unlock_order, smp::LockRank::wait_queue, smp::LockRank::run_queue,
                        &run_queue, 3),
               "out-of-order unlock not reported");
        expect(!run_queue.owned_by_current_cpu(), "out-of-order unlock did not release");
        const auto &cpu = record(3);
        expect(cpu.depth == 2 && cpu.locks[0] == &scheduler && cpu.locks[1] == &wait_queue,
               "out-of-order unlock left the record inconsistent");
        wait_queue.unlock();
        scheduler.unlock();
        expect(reports.count == 1, "release after repair reported");
    });
}

void check_try_lock_order()
{
    on_cpu(0, [] {
        SchedulerLock scheduler;
        RunQueueLock run_queue;
        expect(scheduler.try_lock() && run_queue.try_lock() && reports.count == 0, "ordered try_lock reported");
        run_queue.unlock();
        scheduler.unlock();
        run_queue.lock();
        // It takes the free lock and records the hold, and reports the order.
        expect(scheduler.try_lock(), "try_lock below a held rank failed");
        expect(reported(smp::LockViolationKind::order, smp::LockRank::run_queue, smp::LockRank::scheduler,
                        &scheduler, 0),
               "try_lock below a held rank not reported");
        expect(record(0).depth == 2 && record(0).locks[1] == &scheduler, "try_lock hold not recorded");
        expect(!scheduler.try_lock() && reports.count == 1, "try_lock of an own lock was reported or succeeded");
        scheduler.unlock();
        run_queue.unlock();
        expect(reports.count == 1, "release after try_lock reported");
        // A failed try takes no hold and reports nothing.
        reset_reports();
        run_queue.lock();
        std::thread holder([&] {
            const smp::ScopedHostedCpu scoped(1);
            scheduler.lock();
        });
        holder.join();
        expect(!scheduler.try_lock() && reports.count == 0 && record(0).depth == 1,
               "a failed try_lock reported or recorded");
        run_queue.unlock();
        std::thread releaser([&] {
            const smp::ScopedHostedCpu scoped(1);
            scheduler.unlock();
        });
        releaser.join();
    });
}

void check_nesting_overflow()
{
    on_cpu(1, [] {
        // Nine holds in strictly increasing rank, one past the record.
        static_assert(smp::lock_debug_capacity + 1 == 9);
        smp::RankedSpinLock<smp::LockRank::big_kernel> l0;
        smp::RankedSpinLock<smp::LockRank::ofd_position> l1;
        smp::RankedSpinLock<smp::LockRank::fs_provider> l2;
        SchedulerLock l3;
        RunQueueLock l4;
        WaitQueueLock l5;
        smp::RankedSpinLock<smp::LockRank::ipi> l6;
        smp::RankedSpinLock<smp::LockRank::tlb> l7;
        smp::RankedSpinLock<smp::LockRank::arch_spaces> l8;
        l0.lock(); l1.lock(); l2.lock(); l3.lock(); l4.lock(); l5.lock(); l6.lock(); l7.lock();
        expect(reports.count == 0 && record(1).depth == smp::lock_debug_capacity, "ordered holds reported");
        l8.lock();
        expect(reports.count == 1 && reports.last.kind == smp::LockViolationKind::nesting && reports.last.lock == &l8,
               "nesting overflow not reported");
        l8.unlock(); l7.unlock(); l6.unlock(); l5.unlock(); l4.unlock(); l3.unlock(); l2.unlock(); l1.unlock();
        l0.unlock();
        expect(reports.count == 1, "release after overflow reported");
    });
}

void check_gate_order()
{
    on_cpu(2, [] {
        ModelGate gate;
        smp::RankedSpinLock<smp::LockRank::big_kernel> big;
        smp::RankedSpinLock<smp::LockRank::ofd_position> ofd;
        smp::RankedSpinLock<smp::LockRank::console> console;
        big.lock();
        ofd.lock();
        expect(gate.enter() && reports.count == 0 && smp::lock_debug_depth(2) == 3, "gate hold not recorded in order");
        expect(!gate.enter() && reports.count == 0 && smp::lock_debug_depth(2) == 3,
               "gate reentry was not refused quietly");
        console.lock();
        console.unlock();
        gate.leave();
        ofd.unlock();
        expect(reports.count == 0 && smp::lock_debug_depth(2) == 1, "gate release reported or kept the hold");
        console.lock();
        expect(gate.enter(), "gate refused a free entry");
        expect(reported(smp::LockViolationKind::order, smp::LockRank::console, smp::LockRank::fs_provider, &gate, 2),
               "gate below a held rank not reported");
        gate.leave();
        console.unlock();
        big.unlock();
    });
}

void check_null_handler_counts()
{
    on_cpu(2, [] {
        const auto previous = smp::lock_violation_handler;
        smp::lock_violation_handler = nullptr;
        const auto before = smp::lock_violation_count(2);
        SchedulerLock lock;
        lock.unlock();
        smp::lock_violation_handler = previous;
        expect(smp::lock_violation_count(2) == before + 1 && reports.count == 0, "null handler did not only count");
    });
}

void run_debug_checks()
{
    static_assert(smp::lock_debug_enabled);
    smp::lock_violation_handler = record_report;
    check_order_violation();
    check_self_deadlock();
    check_unlock_not_owner();
    check_lifo_violation();
    check_try_lock_order();
    check_nesting_overflow();
    check_gate_order();
    check_null_handler_counts();
    smp::lock_violation_handler = nullptr;
}

#else

int release_reports = 0;
void count_release_report(const smp::LockViolation &) noexcept
{
    ++release_reports;
}

// Release builds compile every check out: no per-CPU record, no reports, and
// a lock is just its flag and owner word.
void run_release_checks()
{
    static_assert(!smp::lock_debug_enabled);
    static_assert(sizeof(SchedulerLock) == 2 * sizeof(u32)); // Flag and owner word only.
    smp::lock_violation_handler = count_release_report;
    SchedulerLock scheduler;
    RunQueueLock run_queue;
    run_queue.lock();
    scheduler.lock();
    scheduler.unlock();
    run_queue.unlock();
    run_queue.unlock(); // Not owned: silently stores free.
    expect(release_reports == 0 && smp::lock_violation_count(0) == 0, "release build reported a violation");
    smp::lock_violation_handler = nullptr;
}

#endif

} // namespace

int main()
{
    check_hosted_cpu();
    check_guards();
    check_contention();
    check_bkl_accounting();
    check_gate_two_cpus();
#if defined(OK_LOCK_DEBUG)
    run_debug_checks();
    std::printf("OK_SMP_LOCKS scope=model mode=debug checks=%d cpus=%d rounds=%d\n", checks, contention_threads,
                contention_rounds);
#else
    run_release_checks();
    std::printf("OK_SMP_LOCKS scope=model mode=release checks=%d\n", checks);
#endif
    return 0;
}
