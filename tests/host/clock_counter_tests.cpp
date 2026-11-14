#include "../../src/arch/clock_counter.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using ok::u64;
using ok::arch::detail::CounterClock;
using ok::arch::detail::SharedCounterClock;
__extension__ using u128 = unsigned __int128;

namespace
{

void shared_clock()
{
    SharedCounterClock absent;
    assert(absent.resolution() == 0);
    assert(absent.sample(0).status().code() == ok::StatusCode::not_initialized);
    assert(!absent.initialize(0, 0));
    assert(!absent.initialize(~u64{0}, 0));
    assert(!absent.initialize(1000, 0, 32)); // Narrow counters need the accumulating clock.
    assert(!absent.initialized());

    SharedCounterClock fraction;
    assert(fraction.initialize(3000000000, 100));
    assert(fraction.resolution() == 1);
    assert(fraction.sample(101).value() == 0);
    assert(fraction.sample(103).value() == 1);
    assert(fraction.initialize(1, 0)); // A second initialization preserves epoch/rate.
    assert(fraction.sample(106).value() == 2);

    // The conversion is stateless: any read equals the exact quotient.
    for (const auto frequency : {u64{3}, u64{10000000}, u64{400000000}, u64{2400123456}, u64{18000000000}})
    {
        SharedCounterClock exact;
        assert(exact.initialize(frequency, 0));
        u64 cycles = 0, random = 0x591037ab;
        for (unsigned i = 0; i < 10000; ++i)
        {
            random = random * 6364136223846793005ULL + 1;
            cycles += random % 1000000;
            assert(exact.sample(cycles).value() == static_cast<u64>(u128{cycles} * 1000000000 / frequency));
        }
    }

    // A reading behind the floor (another CPU's counter trailing, or behind
    // the epoch) returns the floor; time moves again once it is passed.
    SharedCounterClock skewed;
    assert(skewed.initialize(1000000000, 5000));
    assert(skewed.sample(6000).value() == 1000);
    assert(skewed.sample(5500).value() == 1000);
    assert(skewed.sample(4000).value() == 1000);
    assert(skewed.sample(6500).value() == 1500);

    SharedCounterClock exhausted;
    assert(exhausted.initialize(1, 0));
    assert(exhausted.sample(u64{1} << 62).status().code() == ok::StatusCode::overflow);
    assert(exhausted.sample(1).value() == 1000000000);

    // CPUs whose counters disagree by up to `spread`, each reading its own
    // copy as fast as it can: no CPU ever reads time behind a value any CPU
    // had returned before its read began.
    SharedCounterClock shared;
    constexpr u64 frequency = 1000000000;
    assert(shared.initialize(frequency, 0));
    std::atomic<u64> source{1000};
    std::atomic<u64> published{0};
    std::atomic<bool> backwards{false};
    constexpr unsigned cpus = 4;
    constexpr u64 spread = 300;
    std::vector<std::thread> threads;
    for (unsigned cpu = 0; cpu < cpus; ++cpu)
        threads.emplace_back([&, cpu] {
            u64 last = 0;
            for (unsigned i = 0; i < 200000; ++i)
            {
                const u64 before = published.load(std::memory_order_acquire);
                const u64 counter = source.fetch_add(1 + cpu, std::memory_order_relaxed) + cpu * spread / cpus;
                const auto value = shared.sample(counter);
                if (!value || value.value() < before || value.value() < last)
                    backwards.store(true);
                last = value.value();
                u64 seen = published.load(std::memory_order_relaxed);
                while (seen < last && !published.compare_exchange_weak(seen, last, std::memory_order_acq_rel))
                {
                }
            }
        });
    for (auto &thread : threads)
        thread.join();
    assert(!backwards.load());
}

} // namespace

int main()
{
    CounterClock absent;
    assert(absent.resolution() == 0);
    assert(absent.sample(0).status().code() == ok::StatusCode::not_initialized);
    assert(!absent.initialize(0, 0));
    assert(!absent.initialize(~u64{0}, 0));
    assert(!absent.initialize(1000, 0, 16));
    assert(!absent.initialized());

    CounterClock fraction;
    assert(fraction.initialize(3000000000, 100));
    assert(fraction.resolution() == 1);
    assert(fraction.sample(101).value() == 0);
    assert(fraction.sample(102).value() == 0);
    assert(fraction.sample(103).value() == 1);
    assert(fraction.initialize(1, 0)); // A second initialization preserves epoch/rate.
    assert(fraction.sample(106).value() == 2);

    CounterClock narrow;
    assert(narrow.initialize(1000000, 0xfffffff0, 32));
    assert(narrow.resolution() == 1000);
    assert(narrow.sample(0x10).value() == 32000);
    assert(narrow.sample(0x100000020).value() == 48000);
    CounterClock wide;
    assert(wide.initialize(1000000000, ~u64{0} - 9));
    assert(wide.sample(10).value() == 20);

    // Another CPU's counter behind the last reader's stalls the clock
    // instead of wrapping it forward.
    CounterClock skewed;
    assert(skewed.initialize(1000000000, 5000));
    assert(skewed.sample(6000).value() == 1000);
    assert(skewed.sample(5500).value() == 1000);
    assert(skewed.sample(6000).value() == 1000);
    assert(skewed.sample(7000).value() == 2000);

    CounterClock exhausted;
    assert(exhausted.initialize(1, 0));
    assert(exhausted.sample(u64{1} << 62).status().code() == ok::StatusCode::overflow);
    assert(exhausted.sample(1).value() == 1000000000); // Failure does not publish partial state.
    assert(exhausted.resolution() == 1000000000);

    // Compare repeated reads against an independent wide-integer reference,
    // including frequencies where each individual read loses fractional ns.
    for (const auto frequency : {u64{3}, u64{10000000}, u64{400000000}, u64{2400123456}, u64{18000000000}})
    {
        CounterClock repeated;
        assert(repeated.initialize(frequency, 0));
        u64 cycles = 0, random = 0x591037ab;
        for (unsigned i = 0; i < 10000; ++i)
        {
            random = random * 6364136223846793005ULL + 1;
            cycles += random % 1000000;
            const auto result = repeated.sample(cycles);
            assert(result);
            assert(result.value() == static_cast<u64>(u128{cycles} * 1000000000 / frequency));
        }
    }

    shared_clock();
    std::puts("platform counter conversion: PASS (hosted arithmetic; no hardware execution)");
}
