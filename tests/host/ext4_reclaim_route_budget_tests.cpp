#include <ok/driver/driver.hpp>
#include <ok/smp/smp.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>
#include <vector>

// Fault injection is local to this hosted mechanism test, never a public token.
#define private public
#include <ok/block/sessions.hpp>
#undef private
#include <ok/fs/ext4/alloc.hpp>
#define private public
#include <ok/fs/ext4/recovery.hpp>
#undef private

using namespace ok;
using namespace ok::fs;
using Registry = block::BlockSessions<2>;
using Budget = Ext4ReclaimRouteBudget<Registry::Session>;
using Phase = Ext4ReclaimRoutePhase;
namespace
{
usize checks{}, successful{}, failures{}, callbacks{}, rejected{}, boundary_cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL route budget line=%u: %s\n", unsigned(__LINE__), #expression); std::exit(1); } } while (false)

struct Disk final : driver::BlockDevice
{
    std::vector<std::byte> bytes = std::vector<std::byte>(64 * 512);
    usize reads{}, writes{}, flushes{}, transfer_size{};
    u64 last_sector{};
    Status failure{};
    std::function<void()> callback{}, geometry_callback{};
    driver::BlockGeometry geometry() const override
    {
        if (geometry_callback) geometry_callback();
        return {64, 512, true};
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads; last_sector = first; transfer_size = out.size();
        if (callback) callback();
        if (!failure) return failure;
        std::copy_n(bytes.begin() + first * 512, out.size(), out.begin());
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        ++writes; last_sector = first; transfer_size = in.size();
        if (callback) callback();
        if (!failure) return failure;
        std::copy(in.begin(), in.end(), bytes.begin() + first * 512);
        return {};
    }
    Status flush() override
    {
        ++flushes;
        if (callback) callback();
        return failure;
    }
};
struct Fixture
{
    Disk disk;
    Registry registry;
    Registry::Registration key{};
    Registry::Session session;
    Budget budget;
    std::array<std::byte, 4096> bytes{};
    explicit Fixture(bool reserve = true)
    {
        const auto registration = registry.register_device(disk, 4997, 4096);
        CHECK(registration); key = registration.value();
        CHECK(registry.acquire(key, {4, 32}, block::SessionAccess::writable, session));
        if (reserve) CHECK(budget.reserve_owned_opening(session, this));
    }
    void refined(usize remaining = 0, u32 sequence = 42)
    { CHECK(budget.refine(session, remaining, sequence)); }
    void admit_writer()
    { CHECK(budget.quiescent_handoff(session, this)); CHECK(budget.enter_writer_phase(session, this)); }
    Status read(usize size = 1024)
    {
        const auto began = budget.begin_read(session, 2, size);
        if (!began) return began;
        return budget.finish_read(session, session.read_blocks(2, std::span{bytes}.first(size)));
    }
    Status write()
    {
        const auto began = budget.begin_write(session, 2, 1024);
        if (!began) return began;
        return budget.finish_write(session, session.write_blocks(2, std::span{bytes}.first(1024)));
    }
    Status flush()
    {
        const auto began = budget.begin_flush(session);
        if (!began) return began;
        return budget.finish_flush(session, session.flush());
    }
    void quarantined()
    { CHECK(registry.inspect(key).value().state == block::DeviceState::quarantined); }
};

block::SessionReport stable_report()
{
    block::SessionReport report;
    report.state = block::DeviceState::owned;
    report.identity = 1; report.registration_generation = 1; report.session_serial = 1;
    return report;
}
template <typename Counter> void capacity_boundaries()
{
    constexpr std::array<usize, 8> reads{
        20354895, 20387515, 20415493, 20443471, 20471449, 20499427, 20527405, 20555383};
    const auto max_counter = usize(Counter(~Counter{0}));
    for (usize n = 0; n < reads.size(); ++n)
    {
        const std::array<usize, 3> caps{reads[n], 6 + 15 * n, 5 + 8 * n};
        auto initial = stable_report();
        const u32 delta = 2 + u32(3 * n), last_sequence = ~u32{0} - delta;
        const auto result = Budget::template capacity<Counter>(initial, n, last_sequence);
        CHECK(result);
        CHECK(result.value().reads == caps[0] && result.value().writes == caps[1] &&
              result.value().flushes == caps[2] && result.value().projected_sequence_end == ~u32{0});
        CHECK(Budget::template capacity<Counter>(initial, n, last_sequence + 1).status().code() == StatusCode::overflow);
        for (usize index = 0; index < caps.size(); ++index)
        {
            auto fit = initial;
            if (index == 0) fit.reads = fit.completed_reads = max_counter - caps[index];
            if (index == 1) fit.writes = fit.completed_writes = max_counter - caps[index];
            if (index == 2) fit.flushes = fit.completed_flushes = max_counter - caps[index];
            CHECK(Budget::template capacity<Counter>(fit, n, last_sequence));
            if (index == 0) ++fit.reads, ++fit.completed_reads;
            if (index == 1) ++fit.writes, ++fit.completed_writes;
            if (index == 2) ++fit.flushes, ++fit.completed_flushes;
            CHECK(Budget::template capacity<Counter>(fit, n, last_sequence).status().code() == StatusCode::overflow);
            ++boundary_cases;
        }
    }
}

void capacity_and_states()
{
    static_assert(!std::is_copy_constructible_v<Budget> && !std::is_copy_assignable_v<Budget>);
    static_assert(!std::is_move_constructible_v<Budget> && !std::is_move_assignable_v<Budget>);
    capacity_boundaries<u32>(); capacity_boundaries<u64>();
    auto initial = stable_report();
    CHECK(!Budget::capacity<u8>(initial, 0, 0));
    CHECK(Budget::capacity(initial, 8, 0).status().code() == StatusCode::unsupported);
    for (usize field = 0; field < 9; ++field)
    {
        auto invalid = initial;
        if (field == 0) invalid.state = block::DeviceState::ready;
        if (field == 1) invalid.first_error = StatusCode::io_error;
        if (field == 2) invalid.in_flight = true;
        if (field == 3) invalid.pending_writes = true;
        if (field == 4) ++invalid.reads;
        if (field == 5) ++invalid.writes;
        if (field == 6) ++invalid.flushes;
        if (field == 7) invalid.identity = 0;
        if (field == 8) invalid.session_serial = 0;
        CHECK(!Budget::capacity(invalid, 0, 0));
    }
    {
        Fixture f; CHECK(f.budget.report().phase == Phase::opening);
        CHECK(f.budget.reserve_opening(f.session).code() == StatusCode::already_exists);
        CHECK(f.budget.begin_write(f.session, 2).code() == StatusCode::denied);
        CHECK(f.budget.begin_flush(f.session).code() == StatusCode::denied);
        CHECK(f.read());
        f.refined(7, ~u32{0} - 23);
        CHECK(f.budget.report().capacity.projected_sequence_end == ~u32{0});
        CHECK(f.budget.report().attempts[0] == 1);
        CHECK(f.budget.refine(f.session, 0, 0).code() == StatusCode::already_exists);
        CHECK(f.budget.begin_write(f.session, 2).code() == StatusCode::denied);
        CHECK(f.budget.begin_flush(f.session).code() == StatusCode::denied);
        CHECK(f.disk.writes == 0 && f.disk.flushes == 0);
        CHECK(f.budget.close(f.session));
        CHECK(f.budget.report().phase == Phase::closed);
        CHECK(f.budget.reserve_opening(f.session).code() == StatusCode::already_exists);
        CHECK(f.session.close());
    }
    for (bool members : {false, true})
    {
        Fixture f;
        const auto before = f.session.report().value();
        CHECK(!f.budget.refine(f.session, members ? 8 : 7, members ? 0 : ~u32{0} - 22));
        CHECK(f.budget.report().phase == Phase::failed);
        CHECK(reclaim_history::same_session(before, f.session.report().value()));
        CHECK(f.budget.refine(f.session, 0, 0).code() == StatusCode::already_exists);
        CHECK(f.disk.reads == 0 && f.disk.writes == 0 && f.disk.flushes == 0);
        CHECK(f.session.close());
    }
    {
        Fixture f(false);
        CHECK(f.session.abort(Status::interrupted("before reservation")));
        CHECK(!f.budget.reserve_opening(f.session));
        CHECK(f.budget.reserve_opening(f.session).code() == StatusCode::already_exists);
        CHECK(f.disk.reads == 0);
    }
    {
        Fixture f(false);
        CHECK(!f.budget.begin_read(f.session, 2));
        CHECK(f.budget.reserve_opening(f.session).code() == StatusCode::already_exists);
        CHECK(f.disk.reads == 0 && f.budget.report().phase == Phase::failed);
        CHECK(f.session.close());
    }
    {
        Fixture f(false);
        CHECK(f.session.read_blocks(2, std::span{f.bytes}.first(1024)));
        CHECK(f.session.write_blocks(2, std::span{f.bytes}.first(1024)));
        CHECK(f.session.flush()); CHECK(f.budget.reserve_opening(f.session)); f.refined();
        CHECK(f.read());
        CHECK(f.budget.report().attempts == (std::array<usize, 3>{1, 0, 0}));
        CHECK(f.session.report().value().reads == 2 && f.session.report().value().writes == 1 &&
              f.session.report().value().flushes == 1);
        CHECK(f.budget.close(f.session)); CHECK(f.session.close());
    }
}

void actual_success_prefixes()
{
    Fixture f;
    bool fired{};
    f.disk.callback = [&] {
        CHECK(f.budget.in_flight_exact(f.session));
        CHECK(f.budget.report().attempts[0] == f.disk.reads - 1);
        CHECK(f.budget.report().completions[0] == f.disk.reads - 1);
        ++callbacks; fired = true;
    };
    for (usize pass = 0; pass < 3; ++pass)
    {
        CHECK(f.read(1024));
        CHECK(f.disk.last_sector == 6 && f.disk.transfer_size == 1024);
    }
    CHECK(fired); f.refined(0);
    auto diagnostic = f.budget.report();
    diagnostic.capacity.reads = ~usize{0}; diagnostic.phase = Phase::refined;
    diagnostic.attempts = {}; diagnostic.observed.reads = ~usize{0};
    CHECK(diagnostic.capacity.reads == ~usize{0} && diagnostic.attempts[0] == 0);
    CHECK(f.budget.report().capacity.reads == 20354895 && f.budget.report().attempts[0] == 3);
    CHECK(f.budget.begin_write(f.session, 2).code() == StatusCode::denied);
    f.admit_writer();
    f.disk.callback = [&] {
        CHECK(f.budget.in_flight_exact(f.session));
        const auto actual = f.session.report().value();
        const auto report = f.budget.report();
        CHECK(actual.in_flight && report.dispatch_pending);
        CHECK(actual.completed_reads == report.completions[0]);
        CHECK(actual.completed_writes == report.completions[1]);
        CHECK(actual.completed_flushes == report.completions[2]);
        ++callbacks;
    };
    CHECK(f.write()); CHECK(f.write());
    CHECK(f.session.report().value().pending_writes);
    const auto before = f.disk.reads;
    CHECK(f.read().code() == StatusCode::busy && f.disk.reads == before);
    CHECK(f.flush()); CHECK(!f.session.report().value().pending_writes);
    CHECK(f.read());
    for (usize index = 0; index < 4; ++index) CHECK(f.write());
    const auto writes_before = f.disk.writes;
    CHECK(f.write().code() == StatusCode::overflow && f.disk.writes == writes_before);
    CHECK(f.flush()); CHECK(f.flush()); CHECK(f.flush()); CHECK(f.flush());
    const auto flushes_before = f.disk.flushes;
    CHECK(f.flush().code() == StatusCode::overflow && f.disk.flushes == flushes_before);
    CHECK(f.budget.report().attempts == (std::array<usize, 3>{4, 6, 5}));
    CHECK(f.budget.report().completions == f.budget.report().attempts);
    CHECK(f.budget.close(f.session)); CHECK(f.session.close());
    ++successful;
}

void failed_transport_prefixes()
{
    for (usize operation = 0; operation < 3; ++operation)
    {
        Fixture f; f.refined();
        if (operation != 0) f.admit_writer();
        if (operation == 2) CHECK(f.write());
        const auto before = f.session.report().value();
        f.disk.callback = [&] { CHECK(f.budget.in_flight_exact(f.session)); ++callbacks; };
        f.disk.failure = Status::io_error("route injected transport failure");
        const auto status = operation == 0 ? f.read() : operation == 1 ? f.write() : f.flush();
        CHECK(!status); f.quarantined();
        const auto actual = f.session.report().value();
        const auto diagnostic = f.budget.report();
        CHECK(actual.reads == before.reads + (operation == 0));
        CHECK(actual.writes == before.writes + (operation == 1));
        CHECK(actual.flushes == before.flushes + (operation == 2));
        CHECK(actual.completed_reads == before.completed_reads && actual.completed_writes == before.completed_writes &&
              actual.completed_flushes == before.completed_flushes && !actual.in_flight);
        CHECK(actual.pending_writes == (operation != 0));
        CHECK(diagnostic.attempts[operation] == diagnostic.completions[operation] + 1);
        CHECK(diagnostic.phase == Phase::failed && !diagnostic.dispatch_pending);
        CHECK(reclaim_history::same_session(actual, diagnostic.observed));
        const auto io_count = f.disk.reads + f.disk.writes + f.disk.flushes;
        CHECK(!f.read()); CHECK(!f.write()); CHECK(!f.flush());
        CHECK(f.disk.reads + f.disk.writes + f.disk.flushes == io_count);
        ++failures;
    }
}

void all_rows_transport_credit()
{
    for (usize remaining = 0; remaining <= 7; ++remaining)
    {
        Fixture f; f.refined(remaining); f.admit_writer();
        f.disk.callback = [&] { CHECK(f.budget.in_flight_exact(f.session)); ++callbacks; };
        const usize write_cap = 6 + 15 * remaining, flush_cap = 5 + 8 * remaining;
        for (usize index = 0; index < write_cap; ++index) CHECK(f.write());
        const auto write_count = f.disk.writes;
        CHECK(f.write().code() == StatusCode::overflow && f.disk.writes == write_count);
        for (usize index = 0; index < flush_cap; ++index) CHECK(f.flush());
        const auto flush_count = f.disk.flushes;
        CHECK(f.flush().code() == StatusCode::overflow && f.disk.flushes == flush_count);
        CHECK(f.read()); CHECK(f.budget.report().attempts == (std::array<usize, 3>{1, write_cap, flush_cap}));
        CHECK(f.budget.report().completions == f.budget.report().attempts);
        CHECK(f.budget.close(f.session)); CHECK(f.session.close());
        ++successful; boundary_cases += 2;
    }
}

void callback_and_identity_faults()
{
    using Mutation = std::function<void(Fixture &, Fixture &)>;
    const std::array<Mutation, 12> mutations{
        [](auto &f, auto &) { ++f.budget.control_.first_sector; },
        [](auto &f, auto &) { ++f.budget.control_.transfer_bytes; },
        [](auto &f, auto &) { f.budget.control_.operation = Ext4ReclaimRouteOperation::flush; },
        [](auto &f, auto &) { ++f.budget.control_.attempts[0]; },
        [](auto &f, auto &) { ++f.budget.progress_.completed_reads; },
        [](auto &f, auto &) { ++f.budget.initial_.reads; },
        [](auto &f, auto &) { --f.budget.geometry_.block_count; },
        [](auto &f, auto &) { ++f.budget.sealed_.projected_sequence_end; },
        [](auto &f, auto &) { ++f.budget.flight_.completed_reads; },
        [](auto &f, auto &foreign) { f.budget.original_ = &foreign.session; },
        [](auto &f, auto &) { f.budget.control_.writer = true; },
        [](auto &f, auto &) { ++f.budget.control_.first_sector; ++f.budget.sealed_.first_sector; },
    };
    for (const auto &mutate : mutations)
    {
        Fixture f, foreign; f.refined();
        bool fired{};
        f.disk.callback = [&] { CHECK(f.budget.in_flight_exact(f.session)); mutate(f, foreign); fired = true; };
        CHECK(!f.read()); CHECK(fired); f.quarantined();
        CHECK(foreign.session.report().value().state == block::DeviceState::owned);
        CHECK(foreign.budget.close(foreign.session)); CHECK(foreign.session.close());
        CHECK(f.disk.reads == 1 && f.disk.writes == 0 && f.disk.flushes == 0);
        ++rejected;
    }
    for (usize call = 0; call < 5; ++call)
    {
        Fixture f; f.refined();
        f.disk.callback = [&] {
            Status reentered;
            if (call == 0) reentered = f.budget.begin_read(f.session, 4);
            if (call == 1) reentered = f.budget.reserve_opening(f.session);
            if (call == 2) reentered = f.budget.refine(f.session, 0, 42);
            if (call == 3) reentered = f.budget.finish_read(f.session, {});
            if (call == 4) reentered = f.budget.close(f.session);
            CHECK(!reentered);
        };
        CHECK(!f.read()); f.quarantined();
        CHECK(f.disk.reads == 1 && f.disk.writes == 0 && f.disk.flushes == 0);
        CHECK(f.budget.report().observed.reads == 1 && f.budget.report().observed.completed_reads == 0);
        ++rejected;
    }
    {
        Fixture f, foreign;
        CHECK(!f.budget.begin_read(foreign.session, 2)); f.quarantined();
        CHECK(foreign.session.report().value().state == block::DeviceState::owned);
        CHECK(f.disk.reads == 0 && foreign.disk.reads == 0);
        CHECK(foreign.budget.close(foreign.session)); CHECK(foreign.session.close()); ++rejected;
    }
    {
        Fixture f, foreign;
        CHECK(f.budget.begin_read(f.session, 2));
        const auto transport = f.session.read_blocks(2, std::span{f.bytes}.first(1024));
        CHECK(transport); CHECK(!f.budget.finish_read(foreign.session, transport)); f.quarantined();
        CHECK(foreign.session.report().value().state == block::DeviceState::owned);
        CHECK(foreign.budget.close(foreign.session)); CHECK(foreign.session.close()); ++rejected;
    }
    {
        Fixture f;
        CHECK(f.session.read_blocks(2, std::span{f.bytes}.first(1024)));
        CHECK(!f.budget.begin_read(f.session, 2)); f.quarantined();
        CHECK(f.disk.reads == 1); ++rejected;
    }
    {
        Fixture f;
        CHECK(!f.budget.finish_read(f.session, {})); f.quarantined(); CHECK(f.disk.reads == 0); ++rejected;
    }
}

void session_bounds_faults()
{
    const auto mutate = [](Fixture &f, bool origin) {
        if (origin) ++f.session.first_;
        else --f.session.geometry_.block_count;
    };
    const auto begin = [](Fixture &f, usize operation) {
        return operation == 0 ? f.budget.begin_read(f.session, 2, 1024) :
            operation == 1 ? f.budget.begin_write(f.session, 2, 1024) : f.budget.begin_flush(f.session);
    };
    const auto finish = [](Fixture &f, usize operation, Status transport) {
        return operation == 0 ? f.budget.finish_read(f.session, transport) :
            operation == 1 ? f.budget.finish_write(f.session, transport) : f.budget.finish_flush(f.session, transport);
    };
    for (bool origin : {false, true})
        for (usize operation = 0; operation < 3; ++operation)
        {
            Fixture f; f.refined();
            if (operation != 0) f.admit_writer();
            const auto bounds = f.session.bounds();
            const auto before = f.session.report().value();
            const auto route = f.budget.report();
            CHECK(bounds.first == 4 && bounds.count == 32);
            mutate(f, origin);
            CHECK(f.session.bounds().first == bounds.first + usize(origin));
            CHECK(f.session.bounds().count == bounds.count - usize(!origin));
            CHECK(reclaim_history::same_session(before, f.session.report().value()));
            CHECK(begin(f, operation).code() == StatusCode::fault);
            f.quarantined();
            const auto actual = f.session.report().value();
            const auto diagnostic = f.budget.report();
            CHECK(!f.disk.reads && !f.disk.writes && !f.disk.flushes);
            CHECK(actual.first_error == StatusCode::fault && !actual.in_flight);
            CHECK(actual.reads == before.reads && actual.writes == before.writes && actual.flushes == before.flushes);
            CHECK(actual.completed_reads == before.completed_reads && actual.completed_writes == before.completed_writes &&
                  actual.completed_flushes == before.completed_flushes);
            CHECK(diagnostic.phase == Phase::failed && !diagnostic.dispatch_pending);
            CHECK(diagnostic.attempts == route.attempts && diagnostic.completions == route.completions);
            CHECK(reclaim_history::same_session(actual, diagnostic.observed));
            ++rejected;
        }
    for (bool geometry_callback : {false, true})
        for (bool origin : {false, true})
            for (usize operation = 0; operation < 3; ++operation)
            {
                Fixture f; f.refined();
                if (operation != 0) f.admit_writer();
                if (operation == 2) CHECK(f.write());
                const auto bounds = f.session.bounds();
                const auto before = f.session.report().value();
                const auto route = f.budget.report();
                const std::array<usize, 3> devices{f.disk.reads, f.disk.writes, f.disk.flushes};
                bool fired{};
                const auto callback = [&] {
                    if (fired) return;
                    CHECK(f.budget.in_flight_exact(f.session));
                    const auto actual = f.session.report().value();
                    CHECK(actual.in_flight && f.budget.report().dispatch_pending);
                    CHECK(f.session.bounds().first == bounds.first && f.session.bounds().count == bounds.count);
                    mutate(f, origin); fired = true; ++callbacks;
                };
                if (geometry_callback) f.disk.geometry_callback = callback;
                else f.disk.callback = callback;
                CHECK(begin(f, operation));
                // The raw geometry callback returns the same shape. Only the
                // borrowed Session guard can detect its changed partition bounds.
                const auto transport = operation == 0 ?
                    f.session.read_blocks_guarded(2, std::span{f.bytes}.first(1024), f.budget) :
                    operation == 1 ? f.session.write_blocks_guarded(2, std::span{f.bytes}.first(1024), f.budget) :
                    f.session.flush_guarded(f.budget);
                CHECK(fired && transport.code() == StatusCode::fault);
                f.quarantined();
                const auto actual = f.session.report().value();
                CHECK(actual.first_error == StatusCode::fault && !actual.in_flight);
                CHECK(actual.reads == before.reads + usize(operation == 0));
                CHECK(actual.writes == before.writes + usize(operation == 1));
                CHECK(actual.flushes == before.flushes + usize(operation == 2));
                CHECK(actual.completed_reads == before.completed_reads && actual.completed_writes == before.completed_writes &&
                      actual.completed_flushes == before.completed_flushes);
                CHECK(actual.pending_writes == (operation != 0));
                CHECK(f.disk.reads == devices[0] + usize(!geometry_callback && operation == 0));
                CHECK(f.disk.writes == devices[1] + usize(!geometry_callback && operation == 1));
                CHECK(f.disk.flushes == devices[2] + usize(!geometry_callback && operation == 2));
                CHECK(f.budget.report().phase == route.phase && f.budget.report().dispatch_pending);
                CHECK(finish(f, operation, transport).code() == StatusCode::fault);
                const auto diagnostic = f.budget.report();
                auto attempts = route.attempts;
                ++attempts[operation];
                CHECK(diagnostic.phase == Phase::failed && !diagnostic.dispatch_pending);
                CHECK(diagnostic.attempts == attempts && diagnostic.completions == route.completions);
                CHECK(reclaim_history::same_session(actual, diagnostic.observed));
                const auto count = f.disk.reads + f.disk.writes + f.disk.flushes;
                CHECK(!f.read() && !f.write() && !f.flush());
                CHECK(f.disk.reads + f.disk.writes + f.disk.flushes == count);
                ++rejected;
            }
    std::printf("PASS route Session bounds controls: idle=6 transport_callback=6 same_shape_geometry_callback=6 guarded_completion=0 pending_retired=1\n");
}

// Controlled counter model skips millions of prior READs only for last-fit
// arithmetic. All callback and failure prefix cases above use BlockSessions.
struct CounterSession
{
    block::SessionReport counters = stable_report();
    driver::BlockGeometry shape{32, 512, true};
    Result<block::SessionReport> report() const { return counters; }
    driver::BlockGeometry geometry() const { return shape; }
    block::SessionBounds bounds() const { return {4, shape.block_count}; }
    Result<usize> transfer_limit() const { return usize(4096); }
    Status abort(Status status)
    {
        counters.state = block::DeviceState::quarantined;
        if (counters.first_error == StatusCode::ok) counters.first_error = status.code();
        return {};
    }
};
void exact_route_profile()
{
    {
        Fixture f;
        const auto before = f.session.report().value();
        CHECK(f.budget.refine(f.session, 0, 0).code() == StatusCode::invalid_argument);
        CHECK(reclaim_history::same_session(before, f.session.report().value()));
        CHECK(f.budget.report().phase == Phase::failed && !f.budget.report().dispatch_pending);
        CHECK(!f.disk.reads && !f.disk.writes && !f.disk.flushes);
        CHECK(f.budget.refine(f.session, 0, 42).code() == StatusCode::already_exists);
        CHECK(f.session.close()); ++rejected;
    }
    for (usize sector_size : {usize(1024), usize(4096)})
    {
        CounterSession session; session.shape.block_size = sector_size;
        const auto before = session.counters;
        Ext4ReclaimRouteBudget<CounterSession> budget;
        CHECK(budget.reserve_opening(session).code() == StatusCode::unsupported);
        CHECK(reclaim_history::same_session(before, session.counters));
        CHECK(budget.report().phase == Phase::failed && !budget.report().dispatch_pending);
        CHECK(budget.reserve_opening(session).code() == StatusCode::already_exists); ++rejected;
    }
    {
        Fixture f; f.refined(); f.admit_writer();
        const auto before = f.session.report().value();
        for (const auto &[sector, bytes] : std::array<std::pair<u64, usize>, 4>{{{1, 1024}, {2, 512}, {2, 2048}, {2, 4096}}})
        {
            CHECK(f.budget.begin_write(f.session, sector, bytes).code() == StatusCode::invalid_argument);
            CHECK(reclaim_history::same_session(before, f.session.report().value()));
            CHECK(!f.budget.report().dispatch_pending && !f.disk.writes && !f.disk.flushes);
        }
        CHECK(f.budget.close(f.session) && f.session.close());
    }
}
void last_fit_and_unchanged_denial()
{
    using CounterBudget = Ext4ReclaimRouteBudget<CounterSession>;
    for (bool maximum_origin : {false, true})
    {
        CounterSession session;
        if (maximum_origin) session.counters.reads = session.counters.completed_reads = ~usize{0} - 20555383;
        CounterBudget budget; CHECK(budget.reserve_opening(session)); CHECK(budget.refine(session, 7, 42));
        budget.control_.attempts[0] = budget.control_.completions[0] = 20555382;
        session.counters.reads += 20555382; session.counters.completed_reads += 20555382;
        budget.progress_ = budget.progress_seal_ = session.counters; budget.sync();
        CHECK(budget.begin_read(session, 2, 1024));
        ++session.counters.reads; ++session.counters.completed_reads;
        CHECK(budget.finish_read(session, {}));
        const auto before = session.counters;
        const auto diagnostic = budget.report();
        CHECK(budget.begin_read(session, 2).code() == StatusCode::overflow);
        CHECK(reclaim_history::same_session(session.counters, before));
        CHECK(budget.report().attempts == diagnostic.attempts && !budget.report().dispatch_pending);
        CHECK(budget.close(session)); ++boundary_cases;
    }
    {
        CounterSession session; session.counters.writes = session.counters.completed_writes = ~usize{0} - 110;
        CounterBudget budget;
        CHECK(budget.reserve_opening(session).code() == StatusCode::overflow);
        CHECK(session.counters.state == block::DeviceState::owned);
        CHECK(budget.reserve_opening(session).code() == StatusCode::already_exists); ++boundary_cases;
    }
    {
        CounterSession session; CounterBudget budget;
        CHECK(budget.reserve_opening(session));
        budget.control_.attempts[0] = budget.control_.completions[0] = 20354896;
        session.counters.reads = session.counters.completed_reads = 20354896;
        budget.progress_ = budget.progress_seal_ = session.counters; budget.sync();
        const auto before = session.counters;
        CHECK(budget.refine(session, 0, 42).code() == StatusCode::overflow);
        CHECK(reclaim_history::same_session(before, session.counters)); ++boundary_cases;
    }
    {
        Fixture f;
        const auto before = f.session.report().value();
        for (const auto &[sector, size] : std::array<std::pair<u64, usize>, 9>{{{32, 1024}, {31, 1024}, {2, 0}, {2, 513}, {2, 4608}, {1, 1024}, {2, 512}, {2, 2048}, {2, 4096}}})
        {
            CHECK(f.budget.begin_read(f.session, sector, size).code() == StatusCode::invalid_argument);
            CHECK(f.disk.reads == 0 && reclaim_history::same_session(before, f.session.report().value()));
        }
        CHECK(f.budget.close(f.session)); CHECK(f.session.close());
    }
    {
        Fixture f(false);
        {
            Budget unfinished;
            CHECK(unfinished.reserve_opening(f.session)); CHECK(unfinished.begin_read(f.session, 2));
        }
        f.quarantined(); CHECK(f.disk.reads == 0); ++rejected;
    }
}
void independently_guarded_read_adoption()
{
    {
        Fixture f;
        const auto before = f.session.report().value();
        CHECK(f.session.read_blocks(2, std::span{f.bytes}.first(1024)));
        CHECK(f.session.read_blocks(2, std::span{f.bytes}.first(1024)));
        CHECK(f.budget.adopt_verified_reads(f.session, before, 2));
        CHECK(f.budget.report().attempts == (std::array<usize, 3>{2, 0, 0}));
        f.refined();
        CHECK(f.read());
        CHECK(f.budget.report().attempts[0] == 3 && f.budget.close(f.session));
        CHECK(f.session.close()); ++successful;
    }
    for (bool wrong_origin : {false, true})
    {
        Fixture f;
        auto before = f.session.report().value();
        CHECK(f.session.read_blocks(2, std::span{f.bytes}.first(1024)));
        if (wrong_origin) ++before.reads;
        CHECK(!f.budget.adopt_verified_reads(f.session, before, wrong_origin ? 1 : 2));
        f.quarantined();
        CHECK(f.budget.report().phase == Phase::failed && !f.disk.writes && !f.disk.flushes);
        ++rejected;
    }
}
void same_owner_handoff_controls()
{
    {
        Fixture f; CHECK(f.read()); f.refined(7);
        const auto before = f.session.report().value();
        const auto attempts = f.budget.report().attempts;
        CHECK(f.budget.quiescent_handoff(f.session, &f));
        CHECK(f.budget.report().phase == Phase::handoff);
        CHECK(f.budget.report().capacity.flushes == 61);
        CHECK(reclaim_history::same_session(before, f.session.report().value()));
        CHECK(f.budget.report().attempts == attempts && !f.budget.control_.writer);
        CHECK(f.budget.begin_write(f.session, 2).code() == StatusCode::denied);
        CHECK(f.budget.begin_flush(f.session).code() == StatusCode::denied);
        CHECK(f.read()); // Future canonical misses remain charged to this budget.
        CHECK(f.budget.enter_writer_phase(f.session, &f));
        CHECK(f.flush()); CHECK(f.budget.close(f.session)); CHECK(f.session.close());
        ++successful;
    }
    for (usize control = 0; control < 10; ++control)
    {
        Fixture f, decoy;
        if (control != 0) f.refined();
        if (control == 2) CHECK(f.budget.begin_read(f.session, 2));
        if (control == 3) CHECK(f.budget.close(f.session));
        if (control == 4) { CHECK(f.budget.quiescent_handoff(f.session, &f)); }
        if (control == 5) f.budget.owner_ = &decoy;
        if (control == 6) f.budget.owner_seal_ = &decoy;
        if (control == 7) f.budget.handoff_attempted_ = true;
        if (control == 9) { f.budget.control_.writer = true; f.budget.sync(); }
        const auto count = f.disk.reads + f.disk.writes + f.disk.flushes;
        const auto status = control == 8 ? f.budget.enter_writer_phase(f.session, &f) :
            f.budget.quiescent_handoff(f.session, control == 1 ? static_cast<const void *>(&decoy) : &f);
        CHECK(!status); f.quarantined();
        CHECK(f.disk.reads + f.disk.writes + f.disk.flushes == count);
        CHECK(decoy.session.report().value().state == block::DeviceState::owned);
        CHECK(decoy.budget.close(decoy.session)); CHECK(decoy.session.close()); ++rejected;
    }
    {
        Fixture f(false); CHECK(f.budget.reserve_opening(f.session)); f.refined();
        CHECK(!f.budget.quiescent_handoff(f.session, &f)); f.quarantined();
        CHECK(!f.disk.reads && !f.disk.writes && !f.disk.flushes); ++rejected;
    }
    {
        Fixture f; f.refined(); CHECK(f.budget.quiescent_handoff(f.session, &f));
        Fixture decoy;
        CHECK(!f.budget.enter_writer_phase(f.session, &decoy)); f.quarantined();
        CHECK(decoy.budget.close(decoy.session)); CHECK(decoy.session.close()); ++rejected;
    }
    // The extra barrier must be reserved before the first opening READ.
    {
        CounterSession session; session.counters.flushes = session.counters.completed_flushes = ~usize{0} - 60;
        Ext4ReclaimRouteBudget<CounterSession> budget;
        CHECK(budget.reserve_owned_opening(session, &session).code() == StatusCode::overflow);
        CHECK(session.counters.state == block::DeviceState::owned && !session.counters.reads); ++boundary_cases;
    }
    std::printf("PASS same-Owner route handoff: opening_flush_cap=61 active_flush_cap=5 one_shot=1 no_transport_handoff=1 report_authority=0\n");
}
} // namespace
int main()
{
    capacity_and_states(); actual_success_prefixes(); failed_transport_prefixes(); all_rows_transport_credit();
    callback_and_identity_faults(); last_fit_and_unchanged_denial(); exact_route_profile();
    session_bounds_faults();
    independently_guarded_read_adoption();
    same_owner_handoff_controls();
    std::printf("PASS reclaim route accounting hosted checks=%zu successes=%zu transport_failures=%zu callbacks=%zu rejected=%zu boundary_cases=%zu writer_gate=closed native=0\n",
                checks, successful, failures, callbacks, rejected, boundary_cases);
}
