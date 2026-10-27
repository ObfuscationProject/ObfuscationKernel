#include <ok/fs/ext4/mount.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;
using Budget = Ext4MountedTypedBudget;
using Route = Ext4MountedTypedRoute;
using Origin = Ext4MountedTypedOrigin;
namespace
{
usize checks{}, counter_controls{}, sequence_controls{}, directory_controls{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted typed budget line=%u: %s\n", unsigned(__LINE__), #expression); std::exit(1); } } while (false)
struct Device final : driver::BlockDevice
{
    bool fail_read{};
    driver::BlockGeometry geometry() const override { return {64, 512, true}; }
    Status read_blocks(u64, std::span<std::byte> out) override
    { return fail_read ? Status::io_error("budget model genuine read failure") : (out.empty() ? Status::invalid_argument("empty read") : Status{}); }
    Status write_blocks(u64, std::span<const std::byte>) override { return {}; }
    Status flush() override { return {}; }
};
struct Fixture
{
    Device device{};
    block::BlockSessions<1> service{};
    block::BlockSessions<1>::Session session{};
    Fixture()
    {
        const auto key = service.register_device(device, 53); CHECK(key);
        CHECK(service.acquire(key.value(), {0, 64}, block::SessionAccess::writable, session));
    }
};
block::SessionReport stable()
{
    static Fixture fixture;
    const auto actual = fixture.session.report(); CHECK(actual);
    return actual.value();
}
constexpr std::array routes{Route::zero_fresh, Route::zero_recovery, Route::zero_noop,
                            Route::allocated_fresh, Route::allocated_recovery};
constexpr std::array<u64, 5> reads4{2917723656, 2917720034, 3617, 2917723695, 2917720049};
constexpr std::array<u64, 5> writes{13, 5, 0, 14, 5}, flushes{6, 3, 0, 7, 3};
constexpr std::array<u64, 5> reads32{14247639048, 14247620558, 18709, 14247639115, 14247620573};
static_assert(std::is_same_v<decltype(block::SessionReport::reads), block::SessionCounter>);
static_assert(std::is_same_v<block::BlockSessions<1>::Session::counter_type, block::SessionCounter>);

void golden_routes()
{
    const auto initial = stable();
    const auto bound4 = Budget::bounds(4, 32768), bound32 = Budget::bounds(32, 262144);
    CHECK(bound4); CHECK(bound32);
    CHECK(bound4.value().decode == 18); CHECK(bound4.value().view_open == 1062);
    CHECK(bound4.value().stored_metadata == 2326); CHECK(bound4.value().strict_empty == 2917705839);
    CHECK(bound4.value().mutation_suffix == 2917709265); CHECK(bound4.value().startup_suffix == 2917709283);
    CHECK(bound32.value().decode == 130); CHECK(bound32.value().strict_empty == 14247546611);
    CHECK(bound32.value().mutation_suffix == 14247564961); CHECK(bound32.value().startup_suffix == 14247565091);
    for (usize index = 0; index < routes.size(); ++index)
    {
        auto four = Budget::capacity<u32>(initial, 4, 32768, Origin::mutation, routes[index]);
        auto large = Budget::capacity<u64>(initial, 32, 262144, Origin::mutation, routes[index]);
        CHECK(four); CHECK(large);
        CHECK(four.value().owner_prefix_reads == 9 && large.value().owner_prefix_reads == 9);
        CHECK(four.value().reads == reads4[index]);
        CHECK(large.value().reads == reads32[index]);
        CHECK(four.value().writes == writes[index]); CHECK(four.value().flushes == flushes[index]);
        auto narrow = Budget::capacity<u32>(initial, 32, 262144, Origin::mutation, routes[index]);
        CHECK(bool(narrow) == (routes[index] == Route::zero_noop));
        if (!narrow) CHECK(narrow.status().code() == StatusCode::overflow);
        auto startup = Budget::capacity<u32>(initial, 4, 32768, Origin::startup, routes[index]);
        const bool recovery = routes[index] == Route::zero_recovery || routes[index] == Route::allocated_recovery;
        CHECK(bool(startup) == recovery);
        if (startup) CHECK(startup.value().reads == (routes[index] == Route::zero_recovery ? 2917720020ULL : 2917720035ULL));
        CHECK(!Budget::capacity<u64>(initial, 32, 262144, Origin::startup, routes[index]));
    }
    CHECK(!Budget::bounds(0, 32768)); CHECK(!Budget::bounds(33, 32768)); CHECK(!Budget::bounds(4, 0));
    CHECK(!Budget::capacity(initial, 4, 32768, Origin(9), Route::zero_fresh));
    CHECK(!Budget::capacity(initial, 4, 32768, Origin::mutation, Route(9)));
    CHECK(!Budget::capacity(initial, 4, 32769, Origin::startup, Route::zero_recovery));
    // A maximum block count is clamped without intermediate overflow.
    CHECK(Budget::bounds(32, ~u64{0}).value() == bound32.value());
}

template <block::SessionCounterPolicy Counter> void counter_boundaries()
{
    constexpr u64 limit = block::session_counter_limit<Counter>;
    // Test six independent inert integer headrooms; no report history is seeded.
    for (u64 credit : {u64{3}, u64{3}, u64{5}, u64{5}, u64{7}, u64{7}})
    {
        CHECK(block::session_counter_fits<Counter>(limit - credit, credit)); ++counter_controls;
        CHECK(!block::session_counter_fits<Counter>(limit - credit + 1, credit)); ++counter_controls;
        CHECK(block::session_counter_fits<Counter>(limit, 0)); ++counter_controls;
        if constexpr (sizeof(Counter) < sizeof(block::SessionCounter))
        { CHECK(!block::session_counter_fits<Counter>(limit + 1, credit)); ++counter_controls; }
    }
    const auto actual = stable();
    for (u32 groups = 1; groups <= 32; ++groups)
        for (Route route : routes)
        {
            const auto source = Budget::capacity<u64>(actual, groups, u64{8192} * groups, Origin::mutation, route);
            CHECK(source);
            const auto &capacity = source.value();
            CHECK(bool(Budget::counters<Counter>(actual, capacity)) ==
                (capacity.reads <= limit && capacity.writes <= limit && capacity.flushes <= limit));
            for (u64 credit : {capacity.reads, capacity.reads, capacity.writes,
                               capacity.writes, capacity.flushes, capacity.flushes})
            {
                if (credit > limit)
                { CHECK(!block::session_counter_fits<Counter>(0, credit)); ++counter_controls; continue; }
                CHECK(block::session_counter_fits<Counter>(limit - credit, credit)); ++counter_controls;
                if (credit)
                { CHECK(!block::session_counter_fits<Counter>(limit - credit + 1, credit)); ++counter_controls; }
            }
        }
}

void stable_and_sequence_controls()
{
    const auto preserved = stable();
    Fixture failed;
    failed.device.fail_read = true;
    std::array<std::byte, 512> bytes{};
    CHECK(!failed.session.read_blocks(0, bytes));
    const auto terminal = failed.session.report(); CHECK(terminal);
    CHECK(terminal.value().reads == 1 && terminal.value().completed_reads == 0);
    CHECK(terminal.value().state == block::DeviceState::quarantined);
    auto refused = Budget::capacity<u64>(terminal.value(), 4, 32768, Origin::mutation, Route::zero_fresh);
    CHECK(!refused && refused.status().code() == StatusCode::fault);
    for (Route route : routes)
        for (u32 sequence : {0U, 1U, 2U, 42U, 0xfffffffcU, 0xfffffffdU, 0xfffffffeU, 0xffffffffU})
        {
            const bool fresh = route == Route::zero_fresh || route == Route::allocated_fresh;
            const bool noop = route == Route::zero_noop;
            const bool allowed = sequence && (noop || (fresh ? sequence <= 0xfffffffcU :
                                                        sequence >= 2 && sequence <= 0xfffffffdU));
            auto final = Budget::final_sequence(route, sequence);
            CHECK(bool(final) == allowed); ++sequence_controls;
            if (final) CHECK(final.value() == sequence + (noop ? 0 : fresh ? 2 : 1));
        }
    CHECK(!Budget::final_sequence(Route(9), 42));
    CHECK(Budget::final_sequence(Route::zero_noop, 0xffffffffU).value() == 0xffffffffU);
    CHECK(block::session_counter_fits<>(~u64{0}, 0));
    CHECK(!block::session_counter_fits<>(~u64{0}, 1));
    CHECK(preserved.identity == 53 && preserved.reads == 0 && preserved.writes == 0 && preserved.flushes == 0);

}

void directory_and_checked_math()
{
    for (u32 groups = 1; groups <= 32; ++groups)
    {
        const auto bound = Budget::bounds(groups, 8192ULL * groups).value();
        // Derive the local nested callback cost directly for attempted prefixes,
        // including a prefix whose last block fails. 129 directories are bounded
        // by physically disjoint mappings, with at most 1024 attempts per directory.
        for (u64 prefix = 0; prefix <= 1024; ++prefix)
        {
            const u64 cost = 3 * bound.decode + prefix + 84 * prefix * (2 * bound.decode + prefix);
            const u64 linear = 3 * bound.decode + (1 + 168 * bound.decode + 84 * 1024) * prefix;
            CHECK(cost <= linear); ++directory_controls;
        }
        const u64 all_linear = 387 * bound.decode + (1 + 168 * bound.decode + 84 * 1024) * bound.directory_prefix_sum;
        CHECK(all_linear <= bound.completion);
    }
    u64 out = 77;
    CHECK(!Budget::add(~u64{0}, 1, out)); CHECK(out == 77);
    CHECK(!Budget::multiply(~u64{0}, 2, out)); CHECK(out == 77);
    CHECK(Budget::add(~u64{0} - 1, 1, out)); CHECK(out == ~u64{0});
    CHECK(Budget::multiply(~u64{0}, 1, out)); CHECK(out == ~u64{0});
    CHECK(Budget::multiply(0, ~u64{0}, out)); CHECK(out == 0);
}
} // namespace
int main()
{
    golden_routes(); counter_boundaries<u8>(); counter_boundaries<u16>();
    counter_boundaries<u32>(); counter_boundaries<u64>();
    stable_and_sequence_controls(); directory_and_checked_math();
    std::printf("PASS hosted mounted typed budget checks=%zu counter_controls=%zu sequence_controls=%zu directory_controls=%zu source_only=1 genuine_read_failures=1 native_history_modified=0 independent_acceptance=0\n",
                checks, counter_controls, sequence_controls, directory_controls);
    return 0;
}
