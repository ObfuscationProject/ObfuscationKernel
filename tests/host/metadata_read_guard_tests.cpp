#include <ok/fs/metadata_buffers.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>

using namespace ok;
using namespace ok::block;
using namespace ok::fs;
using Cache = MetadataBuffers<2, 1024, 2>;
using Session = Cache::Registry::Session;

namespace
{
usize checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "metadata read guard:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

bool same(const MetadataOwnerReport &a, const MetadataOwnerReport &b)
{
    return a.state == b.state && a.first_error == b.first_error &&
        a.device_identity == b.device_identity && a.registration_generation == b.registration_generation &&
        a.session_serial == b.session_serial && a.first_sector == b.first_sector && a.sectors == b.sectors &&
        a.blocks == b.blocks && a.read_pins == b.read_pins && a.resident_blocks == b.resident_blocks &&
        a.in_flight == b.in_flight && a.transaction_active == b.transaction_active;
}
bool same(const MetadataBufferReport &a, const MetadataBufferReport &b)
{
    return a.state == b.state && a.block == b.block && a.generation == b.generation &&
        a.transaction_serial == b.transaction_serial && a.read_pins == b.read_pins && a.staged_dirty == b.staged_dirty;
}
bool same(const SessionReport &a, const SessionReport &b)
{
    return a.state == b.state && a.first_error == b.first_error && a.identity == b.identity &&
        a.registration_generation == b.registration_generation && a.session_serial == b.session_serial &&
        a.reads == b.reads && a.completed_reads == b.completed_reads && a.writes == b.writes &&
        a.completed_writes == b.completed_writes && a.flushes == b.flushes && a.completed_flushes == b.completed_flushes &&
        a.in_flight == b.in_flight && a.pending_writes == b.pending_writes;
}

struct Disk final : driver::BlockDevice
{
    std::array<std::byte, 64 * 512> bytes{};
    usize reads{}, writes{}, flushes{};
    u64 last{};
    Status failure{};
    std::function<void()> callback;
    driver::BlockGeometry geometry() const override { return {71, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads; last = first;
        CHECK(first >= 7 && first < 71 && out.size() == 1024);
        CHECK(out.size() <= bytes.size() - (first - 7) * 512);
        if (callback) callback();
        std::copy_n(bytes.begin() + (first - 7) * 512, out.size(), out.begin());
        return failure;
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::fault("unexpected guarded cache write"); }
    Status flush() override
    { ++flushes; return Status::fault("unexpected guarded cache flush"); }
};

struct Guard
{
    Guard() = default;
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;
    usize begins{}, finishes{};
    u64 sector{};
    Session *session{};
    SessionReport before{}, after{};
    Status begin_status{}, finish_status{}, transport_status{};
    Status begin_read(Session &value, u64 first, usize bytes)
    {
        ++begins; session = &value; sector = first; CHECK(bytes == 1024);
        const auto report = value.report(); CHECK(report);
        before = report.value(); CHECK(!before.in_flight);
        return begin_status;
    }
    Status finish_read(Session &value, Status result)
    {
        ++finishes; CHECK(session == &value);
        const auto report = value.report(); CHECK(report);
        after = report.value(); transport_status = result;
        CHECK(!after.in_flight && after.reads == before.reads + 1);
        CHECK(after.completed_reads == before.completed_reads + (result.ok() ? 1 : 0));
        return finish_status;
    }
};
static_assert(MetadataReadDispatchGuard<Guard, Session>);
static_assert(!MetadataReadDispatchGuard<int, Session>);
static_assert(!std::is_copy_constructible_v<Guard> && !std::is_move_constructible_v<Guard>);

struct Harness
{
    Disk disk;
    Cache::Registry registry;
    Cache cache;
    Cache::Registry::Registration key;
    Harness()
    {
        auto result = registry.register_device(disk, 47, 1024); CHECK(result);
        key = result.value(); CHECK(cache.open(registry, key, {7, 64}, SessionAccess::writable));
    }
    void close()
    { CHECK(cache.close()); CHECK(registry.detach(key)); CHECK(!disk.writes && !disk.flushes); }
};

void refusal(bool resident, bool exhausted)
{
    Harness h; Cache::Pin seed, denied, retry; Guard guard;
    if (resident) { CHECK(h.cache.pin(1, seed)); CHECK(seed.release()); }
    if (exhausted) h.cache.entries_[0].report.generation = ~u64{0};
    const auto owner = h.cache.report();
    const auto session = h.registry.inspect(h.key).value();
    const std::array entries{h.cache.entries_[0].report, h.cache.entries_[1].report};
    const auto transport = h.disk.reads;
    guard.begin_status = Status::overflow("injected route capacity exhausted");
    CHECK(h.cache.pin(2, denied, guard).code() == StatusCode::overflow);
    CHECK(guard.begins == 1 && !guard.finishes && guard.sector == 4);
    CHECK(!denied.owner_ && !denied.active_ && !denied.generation_);
    CHECK(same(owner, h.cache.report()) && same(session, h.registry.inspect(h.key).value()));
    CHECK(same(entries[0], h.cache.entries_[0].report) && same(entries[1], h.cache.entries_[1].report));
    CHECK(h.disk.reads == transport && !h.disk.writes && !h.disk.flushes);
    guard.begin_status = {};
    CHECK(h.cache.pin(2, denied, guard));
    CHECK(guard.begins == 2 && guard.finishes == 1 && h.disk.reads == transport + 1 && h.disk.last == 11);
    CHECK(denied.report().value().generation == (exhausted ? 1 : entries[0].generation + 1));
    CHECK(h.cache.pin(2, retry, guard));
    CHECK(guard.begins == 2 && guard.finishes == 1 && h.disk.reads == transport + 1);
    CHECK(denied.release() && retry.release()); h.close();
}

void precedence()
{
    Harness h; Guard guard; Cache::Pin first, second, blocked, hit;
    guard.begin_status = Status::overflow("must not be reached");
    CHECK(h.cache.pin(32, first, guard).code() == StatusCode::invalid_argument && !guard.begins);
    CHECK(h.cache.pin(1, first) && h.cache.pin(2, second));
    CHECK(h.cache.pin(1, first, guard).code() == StatusCode::already_exists && !guard.begins);
    CHECK(h.cache.pin(3, blocked, guard).code() == StatusCode::no_memory && !guard.begins);
    CHECK(h.cache.pin(1, hit, guard) && !guard.begins && h.disk.reads == 2);
    CHECK(first.release() && second.release() && hit.release());
    CHECK(h.cache.close());
    CHECK(h.cache.pin(0, blocked, guard).code() == StatusCode::not_initialized && !guard.begins);
    CHECK(h.registry.detach(h.key) && !h.disk.writes && !h.disk.flushes);
}

void failed_finish(bool transport_failure)
{
    Harness h; Guard guard; Cache::Pin pin, denied;
    if (transport_failure) h.disk.failure = Status::io_error("injected transport failure");
    guard.finish_status = Status::fault("injected retained accounting mismatch");
    CHECK(h.cache.pin(1, pin, guard).code() == (transport_failure ? StatusCode::io_error : StatusCode::fault));
    CHECK(guard.begins == 1 && guard.finishes == 1 && h.disk.reads == 1);
    CHECK(guard.transport_status.code() == (transport_failure ? StatusCode::io_error : StatusCode::ok));
    const auto session = h.registry.inspect(h.key).value();
    CHECK(session.state == DeviceState::quarantined && session.reads == 1);
    CHECK(session.completed_reads == (transport_failure ? 0u : 1u));
    CHECK(session.first_error == (transport_failure ? StatusCode::io_error : StatusCode::fault));
    CHECK(h.cache.report().state == MetadataOwnerState::failed && !h.cache.report().read_pins && !h.cache.report().in_flight);
    CHECK(h.cache.entries_[0].report.state == MetadataBufferState::failed && !h.cache.entries_[0].report.read_pins);
    CHECK(!pin.active_ && h.cache.pin(1, denied, guard).code() == StatusCode::denied);
    CHECK(guard.begins == 1 && guard.finishes == 1 && !h.disk.writes && !h.disk.flushes);
}

void callback_reentry()
{
    Harness h; Guard guard; Cache::Pin pin, same_block, another;
    h.disk.callback = [&]
    {
        CHECK(guard.begins == 1 && !guard.finishes && guard.session);
        const auto report = guard.session->report(); CHECK(report);
        CHECK(report.value().in_flight && report.value().reads == 1 && !report.value().completed_reads);
        CHECK(h.cache.pin(1, same_block, guard).code() == StatusCode::busy);
        CHECK(h.cache.pin(2, another, guard).code() == StatusCode::busy);
        CHECK(guard.begins == 1 && !guard.finishes);
    };
    CHECK(h.cache.pin(1, pin, guard));
    CHECK(guard.finishes == 1 && pin.release()); h.close();
}
} // namespace

int main()
{
    refusal(false, false); refusal(true, false); refusal(false, true); refusal(true, true);
    precedence(); failed_finish(false); failed_finish(true); callback_reentry();
    std::printf("PASS hosted metadata miss dispatch guard checks=%zu native=0 writer=0\n", checks);
}
