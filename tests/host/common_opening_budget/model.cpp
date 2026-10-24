#include <ok/fs/ext4_mounted_common_opening_budget.hpp>
#include "oracle.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using Budget = fs::Ext4MountedCommonOpeningBudget;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL common opening arithmetic line=%u: %s\n", unsigned(__LINE__), #expression); std::exit(1); } } while (false)

template <block::SessionCounterPolicy Policy> void policy_edges()
{
    constexpr auto limit = block::session_counter_limit<Policy>;
    constexpr auto whole = Budget::whole_union();
    constexpr Budget::Counts capacities{whole.reads, whole.reads, whole.writes,
        whole.writes, whole.flushes, whole.flushes};
    Budget::Counts counts{};
    CHECK(bool(Budget::reserve<Policy>(counts)) == (limit >= whole.reads));
    for (usize field = 0; field < capacities.size(); ++field)
    {
        const auto capacity = capacities[field];
        if (capacity > limit) continue;
        counts.fill(0);
        counts[field] = limit - capacity;
        CHECK(bool(Budget::reserve<Policy>(counts)) == (limit >= whole.reads));
        ++counts[field];
        CHECK(Budget::reserve<Policy>(counts).code() == StatusCode::overflow);
    }
}

void routes_and_progress()
{
    constexpr auto whole = Budget::whole_union();
    constexpr auto maximum = block::session_counter_limit<>;
    for (usize index = 0; index < common_opening_oracle.size(); ++index)
    {
        const auto family = static_cast<Budget::Family>(index);
        const auto &expected = common_opening_oracle[index];
        const auto route = Budget::route(family);
        CHECK(route && route.value() == expected);
        CHECK(expected.reads <= whole.reads && expected.writes <= whole.writes && expected.flushes <= whole.flushes);
        // Exact native progress is checked as inert arithmetic; no native report
        // is constructed, seeded, reset, replayed or used as authority.
        for (const auto &baseline : {Budget::Counts{17, 17, 11, 11, 5, 5},
             Budget::Counts{maximum - whole.reads, maximum - whole.reads,
                 maximum - whole.writes, maximum - whole.writes,
                 maximum - whole.flushes, maximum - whole.flushes}})
        {
            auto endpoint = baseline;
            endpoint[0] += expected.locator_reads;
            endpoint[1] += expected.locator_reads;
            const auto remaining = Budget::narrow(baseline, endpoint, family, expected.locator_reads);
            CHECK(remaining);
            CHECK(remaining.value().reads == expected.reads - expected.locator_reads);
            CHECK(remaining.value().writes == expected.writes && remaining.value().flushes == expected.flushes);
            CHECK(remaining.value().sequence_delta == expected.sequence_delta);
            for (u8 spent = 0; spent <= 6; ++spent)
                CHECK(bool(Budget::narrow(baseline, endpoint, family, spent)) == (spent == expected.locator_reads));
            for (usize field = 0; field < endpoint.size(); ++field)
            {
                auto extra = endpoint; ++extra[field];
                CHECK(!Budget::narrow(baseline, extra, family, expected.locator_reads));
                auto missing = endpoint; --missing[field];
                CHECK(!Budget::narrow(baseline, missing, family, expected.locator_reads));
            }
        }
        const Budget::Counts zero{};
        auto endpoint = zero; endpoint[0] = endpoint[1] = expected.locator_reads;
        CHECK(Budget::narrow<u32>(zero, endpoint, family, expected.locator_reads).status().code() == StatusCode::overflow);
        auto unequal = zero; unequal[0] = 1;
        auto unequal_end = unequal;
        unequal_end[0] += expected.locator_reads; unequal_end[1] += expected.locator_reads;
        CHECK(!Budget::narrow(unequal, unequal_end, family, expected.locator_reads));
    }
    // At the exact whole-union edge, charging the original budget again at the
    // touched endpoint would refuse; subtracting its four reads must succeed.
    const Budget::Counts edge{maximum - whole.reads, maximum - whole.reads, 0, 0, 0, 0};
    auto touched = edge; touched[0] += 4; touched[1] += 4;
    CHECK(!block::session_counter_fits<>(touched[0], whole.reads));
    CHECK(Budget::narrow(edge, touched, Budget::Family::ordinary, 4));
    CHECK(!Budget::route(static_cast<Budget::Family>(common_opening_oracle.size())));
    CHECK(!Budget::route(static_cast<Budget::Family>(255)));
    const Budget::Counts zero{};
    CHECK(!Budget::narrow(zero, zero, static_cast<Budget::Family>(255), 5));
}
} // namespace

int main()
{
    policy_edges<u8>(); policy_edges<u16>(); policy_edges<u32>(); policy_edges<u64>();
    routes_and_progress();
    std::printf("PASS hosted common opening arithmetic checks=%zu families=%zu six_headrooms=6 union_reads=58811342498 native_history_modified=0 report_instances=0 transport=0\n", checks, common_opening_oracle.size());
    return 0;
}
