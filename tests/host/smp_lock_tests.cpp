// Hosted model test for ranked spin locks (feat/smp-bringup). Threads stand in
// for CPUs through ScopedHostedCpu; no CPU, interrupt mask or IPI executes.
// Built twice: with OK_LOCK_DEBUG (the full suite) and without (release).
#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
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

void check_try_lock_exemption()
{
    on_cpu(0, [] {
        SchedulerLock scheduler;
        RunQueueLock run_queue;
        run_queue.lock();
        expect(scheduler.try_lock(), "try_lock below a held rank failed");
        expect(reports.count == 0, "try_lock was order checked");
        expect(record(0).depth == 2 && record(0).locks[1] == &scheduler, "try_lock hold not recorded");
        expect(!scheduler.try_lock() && reports.count == 0, "try_lock of an own lock was reported or succeeded");
        scheduler.unlock();
        run_queue.unlock();
        expect(reports.count == 0, "release after try_lock reported");
    });
}

void check_nesting_overflow()
{
    on_cpu(1, [] {
        std::array<smp::RankedSpinLock<smp::LockRank::leaf>, smp::lock_debug_capacity + 1> locks;
        for (auto &lock : locks)
            expect(lock.try_lock(), "nesting try_lock failed");
        expect(reports.count == 1 && reports.last.kind == smp::LockViolationKind::nesting &&
                   reports.last.lock == &locks.back(),
               "nesting overflow not reported");
        for (auto it = locks.rbegin(); it != locks.rend(); ++it)
            it->unlock();
        expect(reports.count == 1, "release after overflow reported");
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
    check_try_lock_exemption();
    check_nesting_overflow();
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
