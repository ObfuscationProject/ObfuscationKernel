// Hosted model test for the class B gates (ok/smp/gate.hpp, smp-scaling WP0a).
// Threads stand in for CPUs through ScopedHostedCpu; no CPU, interrupt or IPI
// executes. Built with and without OK_LOCK_DEBUG, under OK_HOSTED_LOCK_MODEL.
#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/gate.hpp"
#include "ok/smp/lock.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace ok;

namespace
{

// Threads check too.
std::atomic<int> checks{0};

void expect(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

template <typename Done> bool eventually(Done done)
{
    for (int spin = 0; spin < 2000000; ++spin)
    {
        if (done())
            return true;
        std::this_thread::yield();
    }
    return done();
}

void wait_until(const std::atomic<bool> &flag)
{
    while (!flag.load())
        std::this_thread::yield();
}

// The spin service a waiter calls on every pass: counted per hosted CPU.
std::array<std::atomic<long>, smp::max_cpus> served{};
void count_service()
{
    served[smp::current_cpu()].fetch_add(1);
}

using ProviderGate = smp::ServingGate<smp::LockRank::fs_provider>;
using NamespaceGate = smp::SharedGate<smp::LockRank::vfs_namespace>;

constinit ProviderGate global_provider_gate;
constinit NamespaceGate global_namespace_gate;

// Four CPUs through a serving gate: one holder at a time, every increment
// kept, same-CPU re-entry busy and nothing left held or recorded.
void check_serving_race()
{
    long counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false}, reentry{false};
    constexpr int rounds = 20000;
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < 4; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            for (int round = 0; round < rounds; ++round)
            {
                if (!global_provider_gate.enter())
                    reentry.store(true);
                if (inside.fetch_add(1) != 0)
                    overlap.store(true);
                if (global_provider_gate.enter().code() != StatusCode::busy)
                    reentry.store(true);
                ++counter;
                inside.fetch_sub(1);
                global_provider_gate.leave();
            }
        });
    for (auto &thread : threads)
        thread.join();
    expect(!overlap.load() && counter == 4L * rounds, "the serving gate let two CPUs in");
    expect(!reentry.load(), "the serving gate refused a free entry or admitted re-entry");
    expect(!global_provider_gate.held(), "the serving gate was left held");
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
        expect(smp::lock_debug_depth(cpu) == 0 && smp::cpu_mode(cpu) == 0, "a gate CPU kept a hold or a mode bit");
}

// A waiter spins SPINNING outside any lock and serves its interrupts on every
// pass; its wait counts as contention of the class; try_enter never waits.
void check_serving_waiter()
{
    ProviderGate gate;
    std::atomic<bool> held{false}, release{false}, entered{false};
    const auto contended = smp::lock_stat_total(smp::LockStat::fs_provider, true);
    std::thread owner([&] {
        const smp::ScopedHostedCpu scoped(5);
        expect(gate.enter().ok() && gate.owned_by_current_cpu(), "owner entry");
        held.store(true);
        wait_until(release);
        gate.leave();
    });
    wait_until(held);
    std::thread other([&] {
        const smp::ScopedHostedCpu scoped(6);
        expect(!gate.try_enter() && !gate.owned_by_current_cpu(), "try_enter took a held gate");
    });
    other.join();
    std::thread waiter([&] {
        const smp::ScopedHostedCpu scoped(7);
        ProviderGate::Guard guard(gate);
        expect(guard.status().ok() && gate.owned_by_current_cpu(), "waiter entry");
        entered.store(true);
    });
    expect(eventually([] { return (smp::cpu_mode(7) & smp::cpu_mode_spinning) != 0 && served[7].load() > 10; }),
           "a waiting CPU was not SPINNING or did not serve its interrupts");
    expect(!entered.load(), "the waiter got in while the gate was held");
    release.store(true);
    owner.join();
    waiter.join();
    expect(entered.load() && !gate.held() && smp::cpu_mode(7) == 0, "the waiter did not get in or left a bit");
    expect(smp::lock_stat_total(smp::LockStat::fs_provider, true) == contended + 1, "gate contention not counted");
    std::thread free_try([&] {
        const smp::ScopedHostedCpu scoped(6);
        expect(gate.try_enter() && gate.owned_by_current_cpu() && smp::lock_debug_depth(6) == (smp::lock_debug_enabled ? 1u : 0u),
               "try_enter of a free gate");
        expect(gate.enter().code() == StatusCode::busy, "re-entry after try_enter was not busy");
        gate.leave();
    });
    free_try.join();
    expect(!gate.held(), "try_enter hold left behind");
}

// Readers share the gate: two CPUs inside at once.
void check_shared_readers()
{
    NamespaceGate gate;
    std::atomic<int> inside{0};
    std::atomic<bool> both{false}, release{false};
    std::vector<std::thread> readers;
    for (int cpu = 0; cpu < 2; ++cpu)
        readers.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            expect(gate.enter_shared().ok(), "shared entry");
            expect(gate.enter_shared().code() == StatusCode::busy && gate.enter_exclusive().code() == StatusCode::busy,
                   "a shared holder re-entered");
            if (inside.fetch_add(1) == 1)
                both.store(true);
            wait_until(release);
            inside.fetch_sub(1);
            gate.leave_shared();
        });
    expect(eventually([&] { return both.load(); }), "two readers were not inside together");
    expect(gate.shared_holders() == 3 && !gate.exclusive_held(), "shared holder mask");
    std::thread writer_try([&] {
        const smp::ScopedHostedCpu scoped(2);
        expect(!gate.try_enter_exclusive() && gate.try_enter_shared(), "try modes beside readers");
        gate.leave_shared();
    });
    writer_try.join();
    release.store(true);
    for (auto &reader : readers)
        reader.join();
    expect(gate.shared_holders() == 0, "readers left a hold");
}

// A waiting writer holds new readers off and gets in once the current readers
// leave; the readers that were held off then get in.
void check_writer_preference()
{
    NamespaceGate gate;
    std::atomic<bool> reading{false}, release_reader{false}, writer_in{false}, release_writer{false};
    std::atomic<bool> late_in{false};
    std::thread reader([&] {
        const smp::ScopedHostedCpu scoped(8);
        expect(gate.enter_shared().ok(), "first reader");
        reading.store(true);
        wait_until(release_reader);
        gate.leave_shared();
    });
    wait_until(reading);
    std::thread writer([&] {
        const smp::ScopedHostedCpu scoped(9);
        expect(gate.enter_exclusive().ok() && gate.owned_exclusive_by_current_cpu(), "writer entry");
        writer_in.store(true);
        expect(gate.enter_shared().code() == StatusCode::busy, "the writer re-entered shared");
        wait_until(release_writer);
        gate.leave_exclusive();
    });
    expect(eventually([] { return (smp::cpu_mode(9) & smp::cpu_mode_spinning) != 0; }), "the writer did not wait");
    std::thread late([&] {
        const smp::ScopedHostedCpu scoped(10);
        expect(gate.enter_shared().ok(), "late reader");
        late_in.store(true);
        gate.leave_shared();
    });
    expect(eventually([] { return (smp::cpu_mode(10) & smp::cpu_mode_spinning) != 0; }),
           "a reader behind a waiting writer did not wait");
    expect(!late_in.load() && !writer_in.load(), "a reader passed a waiting writer");
    release_reader.store(true);
    reader.join();
    expect(eventually([&] { return writer_in.load(); }), "the writer never got in");
    expect(!late_in.load(), "a reader got in beside the writer");
    release_writer.store(true);
    writer.join();
    late.join();
    expect(late_in.load() && gate.shared_holders() == 0 && !gate.exclusive_held(), "the late reader never got in");
}

// Mixed race: writers keep two words equal under the exclusive hold, readers
// must never see them differ; every writer increment is kept.
void check_shared_race()
{
    constexpr int rounds = 5000;
    std::atomic<long> first{0}, second{0};
    std::atomic<int> writers_inside{0}, readers_inside{0};
    std::atomic<bool> torn{false}, overlap{false};
    std::vector<std::thread> threads;
    for (int cpu = 0; cpu < 6; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            const bool writer = cpu < 2;
            for (int round = 0; round < rounds; ++round)
            {
                if (writer)
                {
                    expect(global_namespace_gate.enter_exclusive().ok(), "exclusive entry");
                    if (writers_inside.fetch_add(1) != 0 || readers_inside.load() != 0)
                        overlap.store(true);
                    first.store(first.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    second.store(second.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                    writers_inside.fetch_sub(1);
                    global_namespace_gate.leave_exclusive();
                }
                else
                {
                    expect(global_namespace_gate.enter_shared().ok(), "shared entry");
                    readers_inside.fetch_add(1);
                    if (writers_inside.load() != 0)
                        overlap.store(true);
                    if (first.load(std::memory_order_relaxed) != second.load(std::memory_order_relaxed))
                        torn.store(true);
                    readers_inside.fetch_sub(1);
                    global_namespace_gate.leave_shared();
                }
            }
        });
    for (auto &thread : threads)
        thread.join();
    expect(!overlap.load() && !torn.load(), "a reader overlapped a writer");
    expect(first.load() == 2L * rounds && second.load() == 2L * rounds, "a writer increment was lost");
    expect(global_namespace_gate.shared_holders() == 0 && !global_namespace_gate.exclusive_held(),
           "the shared gate was left held");
    for (smp::CpuId cpu = 0; cpu < 6; ++cpu)
        expect(smp::lock_debug_depth(cpu) == 0 && smp::cpu_mode(cpu) == 0, "a gate CPU kept a hold or a mode bit");
}

#if defined(OK_LOCK_DEBUG)

int reports = 0;
smp::LockViolation last{};
void record_report(const smp::LockViolation &violation) noexcept
{
    ++reports;
    last = violation;
}

// Holds are recorded at the gate's rank and order checked; entering under a
// class A lock counts class_asserts as well; releasing a gate this CPU does
// not hold is reported and changes nothing.
void check_gate_debug()
{
    std::thread thread([] {
        const smp::ScopedHostedCpu scoped(11);
        smp::lock_violation_handler = record_report;
        ProviderGate provider;
        NamespaceGate names;
        smp::RankedSpinLock<smp::LockRank::big_kernel> big;
        big.lock();
        expect(names.enter_shared().ok() && provider.enter().ok() && reports == 0 && smp::lock_debug_depth(11) == 3,
               "ordered gate holds not recorded");
        provider.leave();
        names.leave_shared();
        expect(names.enter_exclusive().ok() && smp::lock_debug_depth(11) == 2, "exclusive hold not recorded");
        names.leave_exclusive();
        big.unlock();
        expect(smp::lock_debug_depth(11) == 0 && reports == 0, "gate releases");

        const auto classes = smp::check_cpus[11].counts[static_cast<usize>(smp::KernelCheck::class_rule)].load();
        smp::RankedSpinLock<smp::LockRank::task> task;
        task.lock();
        expect(provider.enter().ok(), "a free gate refused under a task lock");
        expect(reports == 1 && last.kind == smp::LockViolationKind::order && last.held == smp::LockRank::task &&
                   last.wanted == smp::LockRank::fs_provider,
               "a gate below a held rank not reported");
        expect(smp::check_cpus[11].counts[static_cast<usize>(smp::KernelCheck::class_rule)].load() == classes + 1,
               "a gate entered under a spin lock not counted as a class violation");
        provider.leave();
        task.unlock();
        reports = 0;
        provider.leave();
        names.leave_shared();
        names.leave_exclusive();
        expect(reports == 3 && last.kind == smp::LockViolationKind::not_owner && smp::lock_debug_depth(11) == 0,
               "release of an unheld gate not reported");
        smp::lock_violation_handler = nullptr;
    });
    thread.join();
}

#endif

} // namespace

int main()
{
    static_assert(smp::concurrent_locks, "gate races need OK_HOSTED_LOCK_MODEL");
    smp::spin_service = count_service;
    check_serving_race();
    check_serving_waiter();
    check_shared_readers();
    check_writer_preference();
    check_shared_race();
#if defined(OK_LOCK_DEBUG)
    check_gate_debug();
#endif
    smp::spin_service = nullptr;
    std::printf("OK_SMP_GATES scope=model mode=%s checks=%d\n", smp::lock_debug_enabled ? "debug" : "release",
                checks.load());
    return 0;
}
