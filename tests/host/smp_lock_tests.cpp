// Hosted model test for ranked spin locks (feat/smp-bringup, smp-scaling WP0a).
// Threads stand in for CPUs through ScopedHostedCpu; no CPU, interrupt mask or
// IPI executes. Built with OK_LOCK_DEBUG (the full suite) and without
// (release), each with OK_HOSTED_LOCK_MODEL (FineLock is the spin lock) and
// without (FineLock is an UpLock).
#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_lock.hpp"
#include "ok/smp/lock.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <type_traits>
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

using TaskTableLock = smp::RankedSpinLock<smp::LockRank::task_table>;
using TaskLock = smp::RankedSpinLock<smp::LockRank::task>;
using RunQueueLock = smp::RankedSpinLock<smp::LockRank::run_queue>;
using ConsoleLock = smp::RankedSpinLock<smp::LockRank::console>;

// Constant initialization is the kernel's only option for globals.
constinit TaskTableLock global_task_table_lock;
constinit TaskLock global_task_lock;

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
                    TaskTableLock::Guard guard(global_task_table_lock);
                    if (inside.fetch_add(1, std::memory_order_relaxed) != 0)
                        overlap.store(true);
                    ++counter;
                    inside.fetch_sub(1, std::memory_order_relaxed);
                }
                else
                {
                    smp::RankedSpinGuard guard{global_task_table_lock};
                    smp::RankedSpinGuard nested{global_task_lock};
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
    expect(!global_task_table_lock.owned_by_current_cpu() && global_task_table_lock.try_lock(),
           "contended lock was left held");
    global_task_table_lock.unlock();
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
    TaskTableLock outer;
    TaskLock inner;
    {
        smp::RankedSpinGuard<smp::LockRank::task_table, FakeInterrupts> guard{outer};
        expect(!FakeInterrupts::enabled && outer.owned_by_current_cpu(), "guard did not mask before holding");
        {
            smp::RankedSpinGuard<smp::LockRank::task, FakeInterrupts> nested{inner};
            expect(!FakeInterrupts::enabled && inner.owned_by_current_cpu(), "nested guard did not hold masked");
        }
        expect(!FakeInterrupts::enabled, "nested guard re-enabled interrupts the outer guard masked");
        expect(!inner.owned_by_current_cpu(), "nested guard did not release");
    }
    expect(FakeInterrupts::enabled && !outer.owned_by_current_cpu(), "guard did not restore enabled interrupts");
    FakeInterrupts::enabled = false;
    {
        smp::RankedSpinGuard<smp::LockRank::task_table, FakeInterrupts> guard{outer};
    }
    expect(!FakeInterrupts::enabled, "guard enabled interrupts that were masked on entry");
    expect(FakeInterrupts::saves == 3 && FakeInterrupts::restores == 3, "guard save/restore count mismatch");
    {
        TaskLock::Guard guard(inner);
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
    TaskTableLock lock;
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
    TaskTableLock contended;
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

using FutexBucketLock = smp::RankedSpinLock<smp::LockRank::futex_bucket>;

// Bucket pairs as futex WAKE_OP takes them: the lower index at its own rank,
// the higher through lock_as. Four CPUs over three buckets keep every pair's
// increments and never deadlock; contention is counted under the futex class.
void check_lock_as_contention()
{
    std::array<FutexBucketLock, 3> buckets;
    std::array<long, 3> counters{};
    constexpr int rounds = 20000;
    const auto contended = smp::lock_stat_total(smp::LockStat::futex, true);
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < 4; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(8 + cpu));
            for (int round = 0; round < rounds; ++round)
            {
                const usize first = static_cast<usize>(round + cpu) % 3, second = static_cast<usize>(round + cpu + 1) % 3;
                const usize low = first < second ? first : second, high = first < second ? second : first;
                buckets[low].lock();
                buckets[high].lock_as<smp::LockRank::futex_bucket_second>();
                ++counters[low];
                ++counters[high];
                buckets[high].unlock();
                buckets[low].unlock();
            }
        });
    for (auto &thread : threads)
        thread.join();
    expect(counters[0] + counters[1] + counters[2] == 2L * 4 * rounds, "a bucket pair lost an increment");
    for (auto &bucket : buckets)
    {
        expect(bucket.try_lock(), "a bucket was left held");
        bucket.unlock();
    }
    for (smp::CpuId cpu = 8; cpu < 12; ++cpu)
        expect(smp::lock_debug_depth(cpu) == 0 && smp::cpu_mode(cpu) == 0, "a bucket CPU kept a hold or a mode bit");
    // Deterministic contention: CPU 12 waits through lock_as for a bucket CPU 13 holds.
    std::atomic<bool> held{false}, release{false}, acquired{false};
    std::thread holder([&] {
        const smp::ScopedHostedCpu scoped(13);
        buckets[1].lock();
        held.store(true);
        while (!release.load())
            std::this_thread::yield();
        buckets[1].unlock();
    });
    while (!held.load())
        std::this_thread::yield();
    std::thread waiter([&] {
        const smp::ScopedHostedCpu scoped(12);
        buckets[0].lock();
        buckets[1].lock_as<smp::LockRank::futex_bucket_second>();
        acquired.store(true);
        buckets[1].unlock();
        buckets[0].unlock();
    });
    bool spinning = false;
    for (int spin = 0; spin < 1000000 && !spinning; ++spin)
    {
        spinning = (smp::cpu_mode(12) & smp::cpu_mode_spinning) != 0;
        std::this_thread::yield();
    }
    expect(spinning && !acquired.load(), "a CPU waiting through lock_as was not marked SPINNING or got in");
    release.store(true);
    holder.join();
    waiter.join();
    expect(acquired.load() && smp::lock_stat_total(smp::LockStat::futex, true) > contended,
           "bucket contention was not counted");
}

// FineLock is the spin lock where CPUs run concurrently (this suite builds
// with OK_HOSTED_LOCK_MODEL and without it) and an UpLock elsewhere; both
// fit the run queue's lock shape and the ranked-lock interface.
void check_fine_lock_selection()
{
    using Fine = smp::FineLock<smp::LockRank::run_queue>;
    static_assert(std::is_same_v<Fine, smp::RankedSpinLock<smp::LockRank::run_queue>> == smp::concurrent_locks);
    static_assert(smp::RankedLock<Fine> && Fine::rank == smp::LockRank::run_queue);
#if defined(OK_HOSTED_LOCK_MODEL)
    static_assert(smp::concurrent_locks);
#else
    static_assert(!smp::concurrent_locks);
#endif
    smp::UpLock<smp::LockRank::task> up;
    expect(!up.owned_by_current_cpu(), "a free UpLock read as held");
    expect(!up.lock() && up.owned_by_current_cpu(), "UpLock lock() waited or was not held");
    // A try on a held lock fails in both modes, as on a RankedSpinLock.
    expect(!up.try_lock(), "try_lock of a held UpLock succeeded");
    up.unlock();
    expect(!up.owned_by_current_cpu(), "a released UpLock read as held");
    {
        smp::UpLock<smp::LockRank::task>::Guard guard(up);
        expect(up.owned_by_current_cpu(), "UpLock guard did not hold");
    }
    expect(!up.owned_by_current_cpu(), "UpLock guard did not release");
    expect(up.try_lock() && up.owned_by_current_cpu(), "UpLock try_lock of a free lock failed");
    expect(!up.try_lock(), "a second try_lock of an UpLock succeeded");
    up.unlock();
#if !defined(OK_LOCK_DEBUG)
    static_assert(sizeof(smp::UpLock<smp::LockRank::task>) == 1); // The held byte.
#endif
    // FineLock answers the same in both selections.
    Fine fine;
    expect(!fine.lock() && fine.owned_by_current_cpu(), "an uncontended FineLock waited or was not held");
    expect(!fine.try_lock(), "try_lock of a held FineLock succeeded");
    fine.unlock();
    expect(!fine.owned_by_current_cpu() && fine.try_lock(), "a released FineLock was not free");
    fine.unlock();
}

// Try-only tokens: one holder, never waited for, never recorded; their holder
// may give the kernel lock up and take it again meanwhile.
void check_try_tokens()
{
    smp::TryToken token;
    expect(!token.held() && token.try_lock() && token.owned_by_current_cpu() && token.held(), "token try_lock");
    expect(!token.try_lock(), "a held token was taken again");
    expect(smp::lock_debug_depth(0) == 0, "a token hold was recorded");
    smp::bkl_lock();
    smp::bkl_unlock();
    smp::bkl_lock();
    smp::bkl_unlock();
    std::thread other([&] {
        const smp::ScopedHostedCpu scoped(1);
        expect(!token.try_lock() && !token.owned_by_current_cpu(), "another CPU took or owned a held token");
    });
    other.join();
    token.unlock();
    expect(!token.held() && !token.owned_by_current_cpu(), "token unlock");
    expect(smp::check_held(smp::hotplug_turn, "model hotplug turn") == !smp::lock_debug_enabled,
           "check_held passed a free token in a debug build");
    expect(smp::hotplug_turn.try_lock(), "hotplug turn");
    expect(smp::check_held(smp::hotplug_turn, "model hotplug turn"), "check_held of the held turn");
    smp::hotplug_turn.unlock();
    expect(smp::housekeeping.try_lock() && !smp::housekeeping.try_lock(), "housekeeping token");
    smp::housekeeping.unlock();
    // Mutual exclusion under contention through try_lock only.
    long counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < 4; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            for (int round = 0; round < 20000;)
            {
                if (!token.try_lock())
                    continue;
                if (inside.fetch_add(1) != 0)
                    overlap.store(true);
                ++counter;
                inside.fetch_sub(1);
                token.unlock();
                ++round;
            }
        });
    for (auto &thread : threads)
        thread.join();
    expect(!overlap.load() && counter == 4L * 20000 && !token.held(), "token mutual exclusion");
}

// The frozen check answers false until a runtime installs it.
void check_frozen_hook()
{
    expect(!smp::bkl_thread_frozen(7), "a thread was frozen without a check");
    smp::bkl_frozen_check = [](u64 pid) noexcept { return pid == 7; };
    expect(smp::bkl_thread_frozen(7) && !smp::bkl_thread_frozen(8), "the installed frozen check was not asked");
    smp::bkl_frozen_check = nullptr;
}

// The top-half mode bit nests like SPINNING.
void check_top_half_scope()
{
    {
        const smp::TopHalfScope outer;
        {
            const smp::TopHalfScope inner;
        }
        expect((smp::cpu_mode(0) & smp::cpu_mode_top_half) != 0, "a nested TopHalfScope cleared the outer bit");
    }
    expect(smp::cpu_mode(0) == 0, "TopHalfScope left TOP_HALF set");
}

// The rank table: every class B rank below every class A rank, top halves
// from irq_router up, bottom halves from device up.
void check_rank_table()
{
    static_assert(smp::lock_class(smp::LockRank::big_kernel) == smp::LockClass::serving);
    static_assert(smp::lock_class(smp::LockRank::mm) == smp::LockClass::serving);
    static_assert(smp::lock_class(smp::LockRank::files) == smp::LockClass::spin);
    static_assert(smp::LockRank::vfs_namespace < smp::LockRank::ofd_position &&
                  smp::LockRank::ofd_position < smp::LockRank::fs_provider &&
                  smp::LockRank::fs_provider < smp::LockRank::block_request &&
                  smp::LockRank::block_request < smp::LockRank::mm && smp::LockRank::mm < smp::LockRank::files);
    static_assert(smp::LockRank::futex_bucket < smp::LockRank::futex_bucket_second &&
                  smp::LockRank::futex_bucket_second < smp::LockRank::task_table &&
                  smp::LockRank::task_table < smp::LockRank::device &&
                  smp::LockRank::device < smp::LockRank::wait_hash && smp::LockRank::wait_hash < smp::LockRank::task &&
                  smp::LockRank::task < smp::LockRank::deadline && smp::LockRank::deadline < smp::LockRank::run_queue);
    static_assert(smp::LockRank::page_pool < smp::LockRank::slab && smp::LockRank::slab < smp::LockRank::frame &&
                  smp::LockRank::frame < smp::top_half_floor &&
                  smp::top_half_floor < smp::LockRank::hotplug_request &&
                  smp::LockRank::hotplug_request < smp::LockRank::console &&
                  smp::LockRank::console < smp::LockRank::runtime_status &&
                  smp::LockRank::runtime_status < smp::LockRank::driver_abi);
    static_assert(smp::bottom_half_floor == smp::LockRank::device && smp::top_half_floor == smp::LockRank::irq_router);
    static_assert(smp::lock_stat(smp::LockRank::futex_bucket_second) == smp::LockStat::futex &&
                  smp::lock_stat(smp::LockRank::console) == smp::LockStat::none);
    expect(smp::lock_stat_names[static_cast<usize>(smp::LockStat::frame)] == "frame" &&
               smp::kernel_check_names[static_cast<usize>(smp::KernelCheck::poll_audit_ready)] == "poll_audit_ready",
           "report name tables out of order");
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
        TaskTableLock table;
        TaskLock task;
        RunQueueLock queue;
        table.lock();
        task.lock();
        queue.lock();
        expect(reports.count == 0 && record(0).depth == 3, "ordered acquisition reported");
        queue.unlock();
        task.unlock();
        table.unlock();
        expect(reports.count == 0 && record(0).depth == 0, "ordered release reported");

        const auto before = smp::lock_violation_count(0);
        task.lock();
        table.lock(); // Free, so it proceeds after the report.
        expect(reported(smp::LockViolationKind::order, smp::LockRank::task, smp::LockRank::task_table,
                        &table, 0),
               "reverse order not reported");
        expect(table.owned_by_current_cpu(), "reverse-order lock() did not take a free lock");
        expect(smp::lock_violation_count(0) == before + 1, "violation not counted on its CPU");
        table.unlock();
        task.unlock();
        expect(reports.count == 1, "LIFO release after a reported order reported again");

        // Equal rank is not strictly above: two task locks need a defined order.
        reset_reports();
        TaskLock other;
        task.lock();
        other.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::task, smp::LockRank::task, &other,
                        0),
               "equal rank not reported");
        other.unlock();
        task.unlock();

        // The order check compares against every hold, not just the top.
        reset_reports();
        ConsoleLock console;
        table.lock();
        console.lock();
        expect(task.try_lock(), "try_lock under a higher rank failed");
        expect(reported(smp::LockViolationKind::order, smp::LockRank::console, smp::LockRank::task, &task,
                        0),
               "try_lock below a held rank not reported");
        reset_reports();
        RunQueueLock wait;
        wait.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::console, smp::LockRank::run_queue, &wait, 0),
               "rank below a non-top hold not reported");
        wait.unlock();
        task.unlock();
        console.unlock();
        table.unlock();
        expect(reports.count == 1, "LIFO release reported");
    });
}

void check_self_deadlock()
{
    on_cpu(1, [] {
        TaskTableLock lock;
        lock.lock();
        // Reported before spinning: the second lock() returns instead of hanging.
        lock.lock();
        expect(reported(smp::LockViolationKind::self_deadlock, smp::LockRank::task_table, smp::LockRank::task_table,
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
        TaskTableLock lock;
        lock.unlock(); // Free.
        expect(reported(smp::LockViolationKind::not_owner, smp::LockRank::task_table, smp::LockRank::task_table, &lock,
                        2),
               "unlock of a free lock not reported");
        reset_reports();
        lock.lock();
        std::thread foreign([&] {
            const smp::ScopedHostedCpu scoped(3);
            lock.unlock(); // Held by CPU 2.
            expect(reported(smp::LockViolationKind::not_owner, smp::LockRank::task_table, smp::LockRank::task_table,
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
        TaskTableLock table;
        TaskLock task;
        RunQueueLock queue;
        table.lock();
        task.lock();
        queue.lock();
        task.unlock();
        expect(reported(smp::LockViolationKind::unlock_order, smp::LockRank::run_queue, smp::LockRank::task,
                        &task, 3),
               "out-of-order unlock not reported");
        expect(!task.owned_by_current_cpu(), "out-of-order unlock did not release");
        const auto &cpu = record(3);
        expect(cpu.depth == 2 && cpu.locks[0] == &table && cpu.locks[1] == &queue,
               "out-of-order unlock left the record inconsistent");
        queue.unlock();
        table.unlock();
        expect(reports.count == 1, "release after repair reported");
    });
}

void check_try_lock_order()
{
    on_cpu(0, [] {
        TaskTableLock table;
        TaskLock task;
        expect(table.try_lock() && task.try_lock() && reports.count == 0, "ordered try_lock reported");
        task.unlock();
        table.unlock();
        task.lock();
        // It takes the free lock and records the hold, and reports the order.
        expect(table.try_lock(), "try_lock below a held rank failed");
        expect(reported(smp::LockViolationKind::order, smp::LockRank::task, smp::LockRank::task_table,
                        &table, 0),
               "try_lock below a held rank not reported");
        expect(record(0).depth == 2 && record(0).locks[1] == &table, "try_lock hold not recorded");
        expect(!table.try_lock() && reports.count == 1, "try_lock of an own lock was reported or succeeded");
        table.unlock();
        task.unlock();
        expect(reports.count == 1, "release after try_lock reported");
        // A failed try takes no hold and reports nothing.
        reset_reports();
        task.lock();
        std::thread holder([&] {
            const smp::ScopedHostedCpu scoped(1);
            table.lock();
        });
        holder.join();
        expect(!table.try_lock() && reports.count == 0 && record(0).depth == 1,
               "a failed try_lock reported or recorded");
        task.unlock();
        std::thread releaser([&] {
            const smp::ScopedHostedCpu scoped(1);
            table.unlock();
        });
        releaser.join();
    });
}

void check_nesting_overflow()
{
    on_cpu(1, [] {
        // The deepest chain the lock order plans (a file read whose device
        // wait drains a bottom half, during the migration under the kernel
        // lock) fits the record; one more hold overflows it.
        static_assert(smp::lock_debug_capacity == 12);
        smp::RankedSpinLock<smp::LockRank::big_kernel> l0;
        smp::RankedSpinLock<smp::LockRank::ofd_position> l1;
        smp::RankedSpinLock<smp::LockRank::fs_provider> l2;
        smp::RankedSpinLock<smp::LockRank::block_request> l3;
        smp::RankedSpinLock<smp::LockRank::mm> l4;
        smp::RankedSpinLock<smp::LockRank::files> l5;
        smp::RankedSpinLock<smp::LockRank::device> l6;
        smp::RankedSpinLock<smp::LockRank::wait_hash> l7;
        TaskLock l8;
        RunQueueLock l9;
        smp::RankedSpinLock<smp::LockRank::frame> l10;
        ConsoleLock l11;
        smp::RankedSpinLock<smp::LockRank::runtime_status> l12;
        l0.lock(); l1.lock(); l2.lock(); l3.lock(); l4.lock(); l5.lock(); l6.lock(); l7.lock(); l8.lock();
        l9.lock(); l10.lock(); l11.lock();
        expect(reports.count == 0 && record(1).depth == smp::lock_debug_capacity, "ordered holds reported");
        expect(smp::lock_debug_max_depth(1) == smp::lock_debug_capacity, "deepest nesting not recorded");
        l12.lock();
        expect(reports.count == 1 && reports.last.kind == smp::LockViolationKind::nesting &&
                   reports.last.lock == &l12,
               "nesting overflow not reported");
        l12.unlock(); l11.unlock(); l10.unlock(); l9.unlock(); l8.unlock(); l7.unlock(); l6.unlock();
        l5.unlock(); l4.unlock(); l3.unlock(); l2.unlock(); l1.unlock(); l0.unlock();
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
        TaskTableLock lock;
        lock.unlock();
        smp::lock_violation_handler = previous;
        expect(smp::lock_violation_count(2) == before + 1 && reports.count == 0, "null handler did not only count");
    });
}

// lock_as records the one acquisition at the wanted rank: a same-rank pair is
// in order, the record names the nested rank, an out-of-order release names
// it too, and a plain second lock() of the same rank is still reported.
void check_lock_as()
{
    on_cpu(4, [] {
        FutexBucketLock lower, higher;
        lower.lock();
        higher.lock_as<smp::LockRank::futex_bucket_second>();
        expect(reports.count == 0 && record(4).depth == 2 &&
                   record(4).ranks[1] == smp::LockRank::futex_bucket_second && record(4).locks[1] == &higher,
               "lock_as was reported or not recorded at its rank");
        lower.unlock();
        expect(reported(smp::LockViolationKind::unlock_order, smp::LockRank::futex_bucket_second,
                        smp::LockRank::futex_bucket, &lower, 4),
               "out-of-order release under lock_as not reported");
        higher.unlock();
        expect(record(4).depth == 0 && !lower.owned_by_current_cpu() && !higher.owned_by_current_cpu(),
               "lock_as release");
        reset_reports();
        lower.lock();
        higher.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::futex_bucket, smp::LockRank::futex_bucket,
                        &higher, 4),
               "a same-rank pair without lock_as not reported");
        higher.unlock();
        lower.unlock();
        reset_reports();
        // Self-deadlock through lock_as is refused like lock().
        lower.lock_as<smp::LockRank::futex_bucket_second>();
        lower.lock_as<smp::LockRank::futex_bucket_second>();
        expect(reported(smp::LockViolationKind::self_deadlock, smp::LockRank::futex_bucket_second,
                        smp::LockRank::futex_bucket_second, &lower, 4),
               "lock_as self-deadlock not reported");
        lower.unlock();
        lower.unlock();
        expect(record(4).depth == 0 && !lower.owned_by_current_cpu(), "lock_as self-deadlock release");
    });
}

// An UpLock keeps the order checks of a RankedSpinLock in debug builds.
void check_up_lock()
{
    on_cpu(5, [] {
        smp::UpLock<smp::LockRank::task> task;
        smp::UpLock<smp::LockRank::run_queue> queue;
        task.lock();
        queue.lock();
        expect(reports.count == 0 && record(5).depth == 2, "ordered UpLocks reported");
        expect(!task.try_lock(), "try_lock of a held UpLock succeeded");
        queue.unlock();
        task.unlock();
        queue.lock();
        task.lock();
        expect(reported(smp::LockViolationKind::order, smp::LockRank::run_queue, smp::LockRank::task, &task, 5),
               "UpLock reverse order not reported");
        task.unlock();
        queue.unlock();
        reset_reports();
        task.lock();
        task.lock();
        expect(reported(smp::LockViolationKind::self_deadlock, smp::LockRank::task, smp::LockRank::task, &task, 5),
               "UpLock self-deadlock not reported");
        task.unlock();
        task.unlock();
        expect(!task.owned_by_current_cpu() && record(5).depth == 0, "UpLock release");
        reset_reports();
        task.unlock();
        expect(reported(smp::LockViolationKind::not_owner, smp::LockRank::task, smp::LockRank::task, &task, 5),
               "UpLock unlock of a free lock not reported");
    });
}

// check_held counts a lock the caller does not hold; class rules count a wait
// under a class A lock; a top half counts a rank below irq_router.
void check_assertion_counters()
{
    on_cpu(6, [] {
        auto count = [](smp::KernelCheck check) {
            return smp::check_cpus[6].counts[static_cast<usize>(check)].load();
        };
        TaskLock task;
        expect(!smp::check_held(task, "model unheld") && count(smp::KernelCheck::held) == 1,
               "check_held of a free lock not counted");
        expect(std::string_view{smp::check_cpus[6].first_site.load()} == "model unheld", "first check site");
        task.lock();
        expect(smp::check_held(task, "model held") && count(smp::KernelCheck::held) == 1, "check_held of a hold");
        std::thread other([&] {
            const smp::ScopedHostedCpu scoped(7);
            expect(!smp::check_held(task, "model other"), "check_held passed another CPU's hold");
        });
        other.join();

        // I1/I2: under the task lock a wait is counted; under class B locks only it is not.
        expect(!smp::check_class_wait("model wait under task") && count(smp::KernelCheck::class_rule) == 1,
               "a wait under a spin lock not counted");
        task.unlock();
        smp::RankedSpinLock<smp::LockRank::big_kernel> big;
        smp::RankedSpinLock<smp::LockRank::mm> mm;
        big.lock();
        mm.lock();
        expect(smp::check_class_wait("model wait under mm") && count(smp::KernelCheck::class_rule) == 1,
               "a wait under class B locks counted");
        mm.unlock();
        big.unlock();

        // Top halves: irq_router and up are allowed, lower ranks counted.
        {
            const smp::TopHalfScope top_half;
            smp::RankedSpinLock<smp::LockRank::irq_router> router;
            ConsoleLock console;
            router.lock();
            router.unlock();
            console.lock();
            console.unlock();
            expect(count(smp::KernelCheck::top_half) == 0, "a top half's own ranks counted");
            task.lock();
            task.unlock();
            expect(count(smp::KernelCheck::top_half) == 1, "a task lock in a top half not counted");
        }
        task.lock();
        task.unlock();
        expect(count(smp::KernelCheck::top_half) == 1 && reports.count == 0, "a task lock outside a top half");
    });
    expect(smp::check_total(smp::KernelCheck::held) >= 2 && smp::check_total(smp::KernelCheck::class_rule) >= 1,
           "check totals");
}

// Debug builds count every acquisition of a reported class, gates included.
void check_lock_stats()
{
    on_cpu(7, [] {
        const auto tasks = smp::lock_stat_cpus[7].acquired[static_cast<usize>(smp::LockStat::task)].load();
        const auto gates = smp::lock_stat_cpus[7].acquired[static_cast<usize>(smp::LockStat::fs_provider)].load();
        TaskLock task;
        task.lock();
        task.unlock();
        expect(task.try_lock(), "free task lock try");
        task.unlock();
        ModelGate gate;
        expect(gate.enter(), "model gate entry");
        gate.leave();
        expect(smp::lock_stat_cpus[7].acquired[static_cast<usize>(smp::LockStat::task)].load() == tasks + 2 &&
                   smp::lock_stat_cpus[7].acquired[static_cast<usize>(smp::LockStat::fs_provider)].load() == gates + 1,
               "acquisitions not counted per class");
    });
}

void run_debug_checks()
{
    static_assert(smp::lock_debug_enabled);
    smp::lock_violation_handler = record_report;
    check_lock_as();
    check_up_lock();
    check_assertion_counters();
    check_lock_stats();
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
    static_assert(sizeof(TaskTableLock) == 2 * sizeof(u32)); // Flag and owner word only.
    smp::lock_violation_handler = count_release_report;
    TaskTableLock table;
    TaskLock task;
    task.lock();
    table.lock();
    table.unlock();
    task.unlock();
    task.unlock(); // Not owned: silently stores free.
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
    check_lock_as_contention();
    check_fine_lock_selection();
    check_try_tokens();
    check_frozen_hook();
    check_top_half_scope();
    check_rank_table();
#if defined(OK_LOCK_DEBUG)
    run_debug_checks();
    std::printf("OK_SMP_LOCKS scope=model mode=debug locks=%s checks=%d cpus=%d rounds=%d\n",
                smp::concurrent_locks ? "concurrent" : "up", checks, contention_threads, contention_rounds);
#else
    run_release_checks();
    std::printf("OK_SMP_LOCKS scope=model mode=release locks=%s checks=%d\n",
                smp::concurrent_locks ? "concurrent" : "up", checks);
#endif
    return 0;
}
