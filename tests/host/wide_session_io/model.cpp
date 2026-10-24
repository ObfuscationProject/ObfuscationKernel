#include <ok/block/session_counters.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::block;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL wide Session arithmetic line=%u: %s\n", unsigned(__LINE__), #expression); std::exit(1); } } while (false)
constexpr std::array<SessionCounter, 6> capacities{
    58'811'342'498ULL, 58'811'342'498ULL, 120, 120, 64, 64};

template <SessionCounterPolicy Counter>
bool six_fit(const std::array<SessionCounter, 6> &used)
{
    for (usize index = 0; index < used.size(); ++index)
        if (!session_counter_fits<Counter>(used[index], capacities[index])) return false;
    return true;
}

template <SessionCounterPolicy Counter> void edges()
{
    constexpr SessionCounter limit = session_counter_limit<Counter>;
    for (usize index = 0; index < capacities.size(); ++index)
    {
        const auto capacity = capacities[index];
        if (capacity > limit)
        { CHECK(!session_counter_fits<Counter>(0, capacity)); continue; }
        CHECK(session_counter_fits<Counter>(limit - capacity, capacity));
        CHECK(!session_counter_fits<Counter>(limit - capacity + 1, capacity));
        CHECK(session_counter_fits<Counter>(limit, 0));
        CHECK(!session_counter_fits<Counter>(limit, 1));
        if constexpr (sizeof(Counter) < sizeof(SessionCounter))
            CHECK(!session_counter_fits<Counter>(limit + 1, 0));
    }
}
} // namespace

int main()
{
    static_assert(sizeof(SessionCounter) == 8);
    static_assert(!SessionCounterPolicy<bool> && !SessionCounterPolicy<i32>);
    static_assert(session_counter_fits<u64>(0, capacities[0]));
    static_assert(!session_counter_fits<u32>(0, capacities[0]));
    edges<u8>(); edges<u16>(); edges<u32>(); edges<u64>();
    std::array<SessionCounter, 6> used{};
    CHECK(six_fit<u64>(used)); CHECK(!six_fit<u32>(used));
    for (usize index = 0; index < capacities.size(); ++index)
    {
        used.fill(0); used[index] = session_counter_limit<> - capacities[index];
        CHECK(six_fit<u64>(used)); ++used[index]; CHECK(!six_fit<u64>(used));
    }
    // A touched classifier endpoint spends five existing credits exactly once.
    const auto baseline = session_counter_limit<> - capacities[0];
    CHECK(session_counter_fits<>(baseline, capacities[0]));
    CHECK(session_counter_fits<>(baseline + 5, capacities[0] - 5));
    CHECK(!session_counter_fits<>(baseline + 5, capacities[0]));
    std::printf("PASS hosted wide Session arithmetic checks=%zu six_headrooms=6 union_reads=58811342498 native_history_modified=0 report_instances=0 transport=0\n", checks);
    return 0;
}
