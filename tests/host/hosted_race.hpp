#pragma once

// Host threads standing in for CPUs, for the allocator race models built with
// OK_HOSTED_LOCK_MODEL (FineLock is then the real spin lock) and usually
// OK_LOCK_DEBUG (every hold is order checked). Model coverage only: no
// interrupt mask, IPI or hardware memory ordering is exercised.

#include "ok/smp/cpu.hpp"
#include "ok/smp/lock.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace ok::test
{

// Runs body(cpu) for cpu = 1 .. cpus on host threads, each one this CPU for
// the hosted current_cpu(), all released at once.
template <typename Body> void race(unsigned cpus, Body body)
{
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(cpus);
    for (unsigned cpu = 1; cpu <= cpus; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu scoped(static_cast<smp::CpuId>(cpu));
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            body(cpu);
        });
    while (ready.load() != cpus)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();
}

// Lock-order, self-deadlock and nesting violations every CPU recorded (always
// zero without OK_LOCK_DEBUG).
inline u64 lock_violations()
{
    u64 total = 0;
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        total += smp::lock_violation_count(cpu);
    return total;
}

// held_asserts and class_asserts every CPU counted.
inline u64 lock_checks()
{
    return smp::check_total(smp::KernelCheck::held) + smp::check_total(smp::KernelCheck::class_rule) +
           smp::check_total(smp::KernelCheck::window_lock);
}

// Cheap per-thread pseudo random sequence.
struct Sequence
{
    u64 state;
    u32 next()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
};

} // namespace ok::test
