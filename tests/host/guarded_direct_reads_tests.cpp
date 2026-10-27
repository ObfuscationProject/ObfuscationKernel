#include <ok/block/guarded_reads.hpp>
#include <ok/fs/ext4/recovery.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>

using namespace ok;
using Registry = block::BlockSessions<2>;
using Session = Registry::Session;
using Budget = fs::Ext4ReclaimRouteBudget<Session>;

namespace
{
usize checks{}, cases{}, callbacks{};
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL guarded direct read line=%u: %s\n", unsigned(__LINE__), #value); \
    std::exit(1); } } while (false)

struct Disk final : driver::BlockDevice
{
    std::array<std::byte, 64 * 512> bytes{};
    driver::BlockGeometry current{64, 512, true};
    usize reads{}, writes{}, flushes{};
    u64 first{};
    Status failure{};
    std::function<void()> callback{};
    driver::BlockGeometry geometry() const override { return current; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; first = sector;
        if (callback) callback();
        if (!failure) return failure;
        std::copy_n(bytes.begin() + sector * 512, out.size(), out.begin());
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("direct read fixture cannot write"); }
    Status flush() override
    { ++flushes; return Status::denied("direct read fixture cannot flush"); }
};

struct TraceGuard final
{
    explicit TraceGuard(Budget &budget) : budget_(budget) {}
    TraceGuard(const TraceGuard &) = delete;
    TraceGuard &operator=(const TraceGuard &) = delete;
    TraceGuard(TraceGuard &&) = delete;
    TraceGuard &operator=(TraceGuard &&) = delete;
    usize begins{}, finishes{}, actual_bytes{};
    u64 actual_first{};
    Status begin_failure{}, finish_failure{}, observed_transport{};
    std::function<void()> after_begin{};
    Status begin_read(Session &session, u64 first, usize bytes)
    {
        ++begins; actual_first = first; actual_bytes = bytes;
        if (!begin_failure) return begin_failure;
        const auto status = budget_.begin_read(session, first, bytes);
        if (status && after_begin) after_begin();
        return status;
    }
    Status finish_read(Session &session, Status transport)
    {
        ++finishes; observed_transport = transport;
        const auto status = budget_.finish_read(session, transport);
        return finish_failure ? status : finish_failure;
    }
  private:
    Budget &budget_;
};

struct Fixture
{
    Disk disk;
    Registry registry;
    Registry::Registration key{};
    Session session;
    Budget budget;
    TraceGuard guard{budget};
    std::array<std::byte, 2048> output{};
    Fixture()
    {
        for (usize index = 0; index < disk.bytes.size(); ++index)
            disk.bytes[index] = std::byte(index % 251);
        const auto registered = registry.register_device(disk, 5031, 4096);
        CHECK(registered); key = registered.value();
        CHECK(registry.acquire(key, {4, 32}, block::SessionAccess::writable, session));
        CHECK(budget.reserve_opening(session));
        CHECK(budget.refine(session, 0, 42));
    }
    Status read(u64 first = 2, usize bytes = 1024)
    { return block::guarded_read_blocks(session, guard, first, std::span{output}.first(bytes)); }
    void no_writer()
    { CHECK(disk.writes == 0 && disk.flushes == 0); }
    void closed()
    { CHECK(budget.close(session)); CHECK(session.close()); }
};

void successful_reads()
{
    ++cases; Fixture f;
    f.disk.callback = [&] {
        ++callbacks;
        CHECK(f.budget.in_flight_exact(f.session));
        CHECK(f.guard.begins == f.guard.finishes + 1);
    };
    CHECK(f.read()); CHECK(f.read(4));
    CHECK(f.guard.begins == 2 && f.guard.finishes == 2);
    CHECK(f.guard.actual_first == 4 && f.guard.actual_bytes == 1024);
    CHECK(std::equal(f.output.begin(), f.output.begin() + 1024, f.disk.bytes.begin() + 8 * 512));
    const auto actual = f.session.report().value();
    CHECK(actual.reads == 2 && actual.completed_reads == 2 && !actual.pending_writes);
    CHECK(f.budget.report().attempts[0] == 2 && f.budget.report().completions[0] == 2);
    f.no_writer(); f.closed();
}

void refused_reads()
{
    for (usize index = 0; index < 5; ++index)
    {
        ++cases; Fixture f;
        const auto initial = f.session.report().value();
        if (index == 4) f.guard.begin_failure = Status::overflow("refused direct allowance");
        const auto status = f.read(index == 0 ? 1 : index == 1 ? 32 : 2,
                                   index == 2 ? 512 : index == 3 ? 2048 : 1024);
        CHECK(status.code() == (index == 4 ? StatusCode::overflow : StatusCode::invalid_argument));
        CHECK(f.guard.begins == 1 && f.guard.finishes == 0);
        CHECK(f.disk.reads == 0);
        CHECK(fs::reclaim_history::same_session(initial, f.session.report().value()));
        CHECK(!f.budget.report().dispatch_pending && f.budget.report().attempts[0] == 0);
        f.no_writer(); f.closed();
    }
}

void failed_reads()
{
    for (usize index = 0; index < 4; ++index)
    {
        ++cases; Fixture f;
        const bool media_failed = index == 0 || index == 2;
        const bool finish_failed = index == 1 || index == 2;
        const bool geometry_failed = index == 3;
        if (media_failed) f.disk.failure = Status::io_error("direct media failure");
        if (finish_failed) f.guard.finish_failure = Status::fault("direct finish failure");
        if (geometry_failed) f.guard.after_begin = [&] { f.disk.current.block_count = 63; };
        f.disk.callback = [&] { ++callbacks; CHECK(f.budget.in_flight_exact(f.session)); };
        const auto status = f.read();
        CHECK(!status);
        CHECK(status.code() == (finish_failed && !media_failed ? StatusCode::fault : StatusCode::io_error));
        CHECK(f.guard.begins == 1 && f.guard.finishes == 1);
        const auto actual = f.session.report().value();
        CHECK(actual.state == block::DeviceState::quarantined && actual.reads == 1);
        CHECK(actual.completed_reads == (index == 1 ? 1U : 0U));
        CHECK(!actual.in_flight && !actual.pending_writes);
        CHECK(actual.first_error == status.code());
        CHECK(f.disk.reads == (geometry_failed ? 0U : 1U));
        CHECK(f.budget.report().attempts[0] == 1);
        CHECK(f.budget.report().completions[0] == (index == 1 ? 1U : 0U));
        CHECK(f.guard.observed_transport.code() == (index == 1 ? StatusCode::ok : StatusCode::io_error));
        const auto observed = f.session.report().value();
        CHECK(!f.read());
        CHECK(fs::reclaim_history::same_session(observed, f.session.report().value()));
        CHECK(f.guard.begins == 2 && f.guard.finishes == 1);
        f.no_writer();
    }
}

void callback_reentry()
{
    ++cases; Fixture f;
    f.disk.callback = [&] {
        ++callbacks;
        CHECK(f.budget.in_flight_exact(f.session));
        const auto nested = f.read();
        CHECK(nested.code() == StatusCode::fault);
        CHECK(f.guard.begins == 2 && f.guard.finishes == 0);
    };
    CHECK(!f.read());
    CHECK(f.disk.reads == 1 && f.guard.finishes == 1);
    const auto actual = f.session.report().value();
    CHECK(actual.state == block::DeviceState::quarantined && actual.first_error == StatusCode::fault);
    CHECK(actual.reads == 1 && actual.completed_reads == 0 && !actual.in_flight);
    f.no_writer();
}
} // namespace

int main()
{
    static_assert(block::ReadDispatchGuard<TraceGuard, Session>);
    static_assert(!std::is_copy_constructible_v<TraceGuard> && !std::is_move_constructible_v<TraceGuard>);
    static_assert(!std::is_copy_constructible_v<Session> && !std::is_move_constructible_v<Session>);
    successful_reads(); refused_reads(); failed_reads(); callback_reentry();
    std::printf("PASS guarded direct reads checks=%zu cases=%zu callbacks=%zu writer_gate=closed native=0\n",
                checks, cases, callbacks);
}
