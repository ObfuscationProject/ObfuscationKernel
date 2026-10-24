// Public Session model: observations, declared device failures, guard refusals
// and ordinary abort only. No private record or pointer changes are performed.
#include <ok/block/sessions.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
using Registry = block::BlockSessions<2>;
using Session = Registry::Session;
namespace
{
usize checks{}, cases{}, transports{}, observations{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "native return order:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
enum class Kind : u8 { read, write, flush };
struct Trace
{
    std::array<char, 16> events{};
    usize count{};
    Session *session{};
    block::SessionReport before{};
    Kind kind{};
    void append(char event)
    {
        CHECK(count < events.size()); events[count++] = event;
        const auto actual = session->report(); CHECK(actual);
        const auto &report = actual.value();
        CHECK(report.in_flight);
        CHECK(report.reads == before.reads + usize(kind == Kind::read));
        CHECK(report.writes == before.writes + usize(kind == Kind::write));
        CHECK(report.flushes == before.flushes + usize(kind == Kind::flush));
        CHECK(report.completed_reads == before.completed_reads);
        CHECK(report.completed_writes == before.completed_writes);
        CHECK(report.completed_flushes == before.completed_flushes);
        CHECK(report.pending_writes == (before.pending_writes || kind == Kind::write));
        ++observations;
    }
    void exact(const char *expected) const
    { CHECK(count == std::strlen(expected)); CHECK(!std::memcmp(events.data(), expected, count)); }
};
struct Device final : driver::BlockDevice
{
    Trace *trace{};
    mutable usize geometry_calls{};
    usize transport_calls{}, mismatch_at{};
    bool fail{}, abort_on_failure{};
    driver::BlockGeometry geometry() const override
    {
        if (trace) { ++geometry_calls; trace->append('g'); }
        return {trace && geometry_calls == mismatch_at ? 31U : 32U, 512, true};
    }
    Status transport(char event)
    {
        ++transport_calls; ++transports; trace->append(event);
        if (fail)
        {
            if (abort_on_failure) CHECK(trace->session->abort(Status::interrupted("ordinary callback abort")));
            return Status::io_error("declared transport failure");
        }
        return {};
    }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    { CHECK(first == 1 && output.size() == 512); return transport('r'); }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    { CHECK(first == 1 && input.size() == 512); return trace ? transport('w') : Status{}; }
    Status flush() override { return transport('f'); }
};
struct Guard
{
    Trace &trace;
    usize calls{}, refusal{};
    Status in_flight_exact(Session &session)
    {
        CHECK(&session == trace.session); ++calls; trace.append(char('0' + calls));
        return calls == refusal ? Status::denied("deliberate guard refusal") : Status{};
    }
};
void run(Kind kind, bool pending, usize scenario)
{
    ++cases;
    Device disk; Registry registry; Session session; std::array<std::byte, 512> bytes{};
    const auto registered = registry.register_device(disk, 9100 + cases); CHECK(registered);
    CHECK(registry.acquire(registered.value(), {0, 32}, block::SessionAccess::writable, session));
    if (pending) CHECK(session.write_blocks(1, bytes));
    Trace trace; trace.session = &session; trace.before = session.report().value(); trace.kind = kind;
    Guard guard{trace}; disk.trace = &trace;
    // 0 success; 1..4 guard refusals; 5 failed transport with unreachable
    // refusal 3; 6 failed transport plus ordinary public abort; 7/8 declared
    // post/pre geometry mismatch. Every scenario starts with a fresh Session.
    if (scenario >= 1 && scenario <= 4) guard.refusal = scenario;
    if (scenario == 5 || scenario == 6) { disk.fail = true; guard.refusal = 3; }
    if (scenario == 6) disk.abort_on_failure = true;
    if (scenario == 7 || scenario == 8) disk.mismatch_at = scenario == 7 ? 2 : 1;
    const auto status = kind == Kind::read ? session.read_blocks_guarded(1, bytes, guard) :
        kind == Kind::write ? session.write_blocks_guarded(1, bytes, guard) : session.flush_guarded(guard);
    const char event = kind == Kind::read ? 'r' : kind == Kind::write ? 'w' : 'f';
    char successful[] = {'1','g','2',event,'3','g','4',0};
    char after_transport[] = {'1','g','2',event,0};
    char refusal_three[] = {'1','g','2',event,'3',0};
    char post_mismatch[] = {'1','g','2',event,'3','g',0};
    trace.exact(scenario == 1 ? "1" : scenario == 2 ? "1g2" : scenario == 3 ? refusal_three :
        scenario == 5 || scenario == 6 ? after_transport : scenario == 7 ? post_mismatch :
        scenario == 8 ? "1g" : successful);
    CHECK(guard.calls == (scenario == 1 || scenario == 8 ? 1U : scenario == 2 || scenario == 5 || scenario == 6 ?
        2U : scenario == 3 || scenario == 7 ? 3U : 4U));
    CHECK(disk.geometry_calls == (scenario == 1 ? 0U : (scenario >= 2 && scenario <= 3) ||
        scenario == 5 || scenario == 6 || scenario == 8 ? 1U : 2U));
    CHECK(disk.transport_calls == (scenario == 1 || scenario == 2 || scenario == 8 ? 0U : 1U));
    CHECK(status.code() == (scenario == 0 ? StatusCode::ok : scenario <= 4 ? StatusCode::denied : StatusCode::io_error));
    const auto report = session.report().value();
    CHECK(!report.in_flight);
    CHECK(report.reads == trace.before.reads + usize(kind == Kind::read));
    CHECK(report.writes == trace.before.writes + usize(kind == Kind::write));
    CHECK(report.flushes == trace.before.flushes + usize(kind == Kind::flush));
    CHECK(report.completed_reads == trace.before.completed_reads + usize(scenario == 0 && kind == Kind::read));
    CHECK(report.completed_writes == trace.before.completed_writes + usize(scenario == 0 && kind == Kind::write));
    CHECK(report.completed_flushes == trace.before.completed_flushes + usize(scenario == 0 && kind == Kind::flush));
    CHECK(report.pending_writes == (scenario == 0 && kind == Kind::flush ? false : pending || kind == Kind::write));
    CHECK(report.state == (scenario == 0 ? block::DeviceState::owned : block::DeviceState::quarantined));
    CHECK(report.first_error == (scenario == 6 ? StatusCode::interrupted : status.code()));
    disk.trace = nullptr;
    if (scenario == 0)
    {
        // Drain pending writes by an ordinary unguarded flush, then close.
        if (report.pending_writes) { disk.trace = &trace; trace.before = report; trace.kind = Kind::flush;
            CHECK(session.flush()); disk.trace = nullptr; }
        CHECK(session.close()); CHECK(registry.detach(registered.value()));
    }
}
}
int main()
{
    for (Kind kind : {Kind::read, Kind::write, Kind::flush})
        for (bool pending : {false, true})
            for (usize scenario = 0; scenario < 9; ++scenario) run(kind, pending, scenario);
    std::printf("NATIVE_SESSION_RETURN_ORDER cases=%zu checks=%zu transports=%zu observations=%zu\n", cases, checks, transports, observations);
    return 0;
}
