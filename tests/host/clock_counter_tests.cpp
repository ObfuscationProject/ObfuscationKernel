#include "../../src/arch/clock_counter.hpp"

#include <cassert>
#include <cstdio>

using ok::u64;
using ok::arch::detail::CounterClock;
__extension__ using u128 = unsigned __int128;

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

    CounterClock exhausted;
    assert(exhausted.initialize(1, 0));
    assert(exhausted.sample(~u64{0}).status().code() == ok::StatusCode::overflow);
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
    std::puts("platform counter conversion: PASS (hosted arithmetic; no hardware execution)");
}
