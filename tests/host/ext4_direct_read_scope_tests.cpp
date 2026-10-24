// Hosted serialized real Owner/Session model. Compile with -fno-access-control
// solely for independent report, attachment and counter fault injection.
#include "ok/fs/ext4_owned_transaction.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <type_traits>

using namespace ok;
using namespace ok::fs;

namespace
{
usize checks{}, cases{}, geometry_cases{}, flight_cases{}, pending_cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "direct read scope:%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
using Owner = Ext4OwnedTransaction<2, 8>;
using Session = Owner::Registry::Session;

class Disk final : public driver::BlockDevice
{
  public:
    driver::BlockGeometry geometry() const override
    { if (during_geometry) during_geometry(); return {1024, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override
    {
        ++reads; sector = first; size = bytes.size(); output = bytes.data();
        if (during_read) during_read();
        for (auto &byte : bytes) byte = std::byte{0x35};
        return media;
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return write_media; }
    Status flush() override { ++flushes; return Status::fault("direct scope unexpected flush"); }
    usize reads{}, writes{}, flushes{}, size{};
    u64 sector{};
    std::byte *output{};
    Status media{};
    Status write_media{Status::fault("direct scope unexpected write")};
    std::function<void()> during_read{};
    mutable std::function<void()> during_geometry{};
};
struct Authority final
{
    Owner *attached{}, *original{};
    bool enabled{true};
    usize calls{};
    Status operator()()
    {
        ++calls;
        return enabled && attached == original ? Status{} : Status::fault("direct scope original Owner authority differs");
    }
};
using Scope = Ext4DirectReadScope<Session, Authority>;
static_assert(block::ReadDispatchGuard<Scope, Session>);
static_assert(!std::is_copy_constructible_v<Scope> && !std::is_move_constructible_v<Scope>);
static_assert(!std::is_copy_assignable_v<Scope> && !std::is_move_assignable_v<Scope>);

struct Fixture final
{
    Disk disk{}, decoy_disk{};
    Owner::Registry registry{};
    std::unique_ptr<Owner> owner{std::make_unique<Owner>()}, decoy{std::make_unique<Owner>()};
    Owner::Registry::Registration registration{}, decoy_registration{};
    Authority authority{owner.get(), owner.get()};
    std::array<std::byte, 4096> bytes{};
    explicit Fixture(usize limit = 4096)
    {
        const auto key = registry.register_device(disk, 35001, limit); CHECK(key); registration = key.value();
        const auto other = registry.register_device(decoy_disk, 35002, limit); CHECK(other); decoy_registration = other.value();
        CHECK(owner->open(registry, registration, {37, 128}));
        CHECK(decoy->open(registry, decoy_registration, {51, 128}));
        owner->reclaim_io_ = session().report().value();
        decoy->reclaim_io_ = decoy->cache_.session_.report().value();
    }
    Session &session() { return owner->cache_.session_; }
    block::SessionReport &shadow() { return owner->reclaim_io_; }
    block::SessionReport actual() { const auto report = session().report(); CHECK(report); return report.value(); }
    block::SessionReport decoy_report() { const auto report = decoy->cache_.session_.report(); CHECK(report); return report.value(); }
    auto &entry() { return registry.entries_[registration.slot_]; }
    void quiet() { CHECK(!disk.writes && !disk.flushes && !decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes); }
    Status read(u64 first = 3, usize count = 1024)
    { return guarded_ext4_direct_read(session(), shadow(), authority, first, std::span{bytes}.first(count)); }
};
struct Hook final
{
    usize begins{}, finishes{};
    Status begin_status{}, finish_status{}, observed{};
    std::function<void()> at_begin{}, at_finish{};
    Status begin_read(Session &, u64, usize) { ++begins; if (at_begin) at_begin(); return begin_status; }
    Status finish_read(Session &, Status media) { ++finishes; observed = media; if (at_finish) at_finish(); return finish_status; }
};
struct FlightHook final
{
    Hook outer{};
    usize observations{}, refusal{};
    std::function<void(Session &, usize)> at_observation{};
    Status begin_read(Session &session, u64 first, usize bytes)
    { return outer.begin_read(session, first, bytes); }
    Status finish_read(Session &session, Status media)
    { return outer.finish_read(session, media); }
    Status in_flight_exact(Session &session)
    {
        ++observations;
        if (at_observation) at_observation(session, observations);
        return observations == refusal ? Status::denied("external in-flight refusal") : Status{};
    }
};
using Dispatch = Ext4DirectReadDispatch<Scope, Hook, Session>;
static_assert(requires(Session &session, Dispatch &dispatch, std::span<std::byte> bytes)
    { { session.read_blocks_guarded(3, bytes, dispatch) } -> std::same_as<Status>; });

void successful_paused_prefix()
{
    Fixture f;
    const auto before = f.actual(), decoy = f.decoy_report();
    Scope scope(f.session(), f.shadow(), f.authority, 3, 1024);
    CHECK(scope.prepare());
    usize observations{};
    f.disk.during_read = [&] {
        auto pending = before; ++pending.reads; pending.in_flight = true;
        CHECK(reclaim_history::same_session(f.actual(), pending));
        CHECK(reclaim_history::same_session(f.shadow(), before));
        CHECK(scope.in_flight_exact(f.session())); ++observations;
    };
    CHECK(block::guarded_read_blocks(f.session(), scope, 3, std::span{f.bytes}.first(1024)));
    CHECK(reclaim_history::same_session(f.shadow(), before));
    auto completed = before; ++completed.reads; ++completed.completed_reads;
    CHECK(reclaim_history::same_session(f.actual(), completed));
    CHECK(scope.adopt(f.session()));
    CHECK(reclaim_history::same_session(f.shadow(), completed));
    CHECK(observations == 1 && f.disk.reads == 1 && f.disk.sector == 40 && f.disk.size == 1024 && f.disk.output == f.bytes.data());
    CHECK(reclaim_history::same_session(f.decoy_report(), decoy));
    f.quiet(); ++cases;
}
void refusals()
{
    {
        Fixture f; Hook hook; const auto before = f.actual();
        hook.begin_status = Status::denied("external begin refusal");
        CHECK(guarded_ext4_direct_read(f.session(), f.shadow(), f.authority, 3, std::span{f.bytes}.first(1024), hook).code() == StatusCode::denied);
        CHECK(hook.begins == 1 && !hook.finishes && !f.disk.reads);
        CHECK(reclaim_history::same_session(f.actual(), before) && reclaim_history::same_session(f.shadow(), before));
        CHECK(f.read()); CHECK(f.disk.reads == 1); f.quiet(); ++cases;
    }
    for (bool media_failure : {false, true})
    {
        Fixture f; Hook hook; const auto before = f.actual(), decoy = f.decoy_report();
        if (media_failure) f.disk.media = Status::io_error("physical media failure");
        hook.finish_status = Status::denied("external finish refusal");
        const auto result = guarded_ext4_direct_read(f.session(), f.shadow(), f.authority, 3, std::span{f.bytes}.first(1024), hook);
        CHECK(result.code() == (media_failure ? StatusCode::io_error : StatusCode::denied));
        CHECK(hook.begins == 1 && hook.finishes == 1 && hook.observed.code() == f.disk.media.code());
        CHECK(f.disk.reads == 1 && reclaim_history::same_session(f.shadow(), before));
        const auto now = f.actual();
        CHECK(now.state == block::DeviceState::quarantined && !now.in_flight && now.reads == before.reads + 1);
        CHECK(now.completed_reads == before.completed_reads + usize(!media_failure));
        CHECK(now.first_error == result.code() && reclaim_history::same_session(f.decoy_report(), decoy));
        CHECK(!f.read() && f.disk.reads == 1); f.quiet(); ++cases;
    }
}
void shadow_and_attachment_drift()
{
    for (usize stage = 0; stage < 4; ++stage)
    {
        Fixture f; Hook hook; const auto before = f.actual(), decoy = f.decoy_report();
        auto mutate = [&] { if (stage % 2) f.authority.attached = f.decoy.get(); else ++f.shadow().completed_flushes; };
        if (stage < 2) f.disk.during_read = mutate; else hook.at_finish = mutate;
        CHECK(guarded_ext4_direct_read(f.session(), f.shadow(), f.authority, 3, std::span{f.bytes}.first(1024), hook).code() == StatusCode::fault);
        const auto now = f.actual();
        CHECK(f.disk.reads == 1 && now.state == block::DeviceState::quarantined && now.first_error == StatusCode::fault);
        CHECK(now.reads == before.reads + 1 && now.completed_reads == before.completed_reads + usize(stage >= 2));
        CHECK(f.shadow().reads == before.reads && f.shadow().completed_reads == before.completed_reads);
        CHECK(reclaim_history::same_session(f.decoy_report(), decoy)); f.quiet(); ++cases;
    }
    {
        Fixture f; Hook hook; const auto before = f.actual();
        hook.begin_status = Status::denied("external begin refusal");
        hook.at_begin = [&] { ++f.shadow().reads; };
        CHECK(guarded_ext4_direct_read(f.session(), f.shadow(), f.authority, 3, std::span{f.bytes}.first(1024), hook).code() == StatusCode::fault);
        CHECK(!f.disk.reads && !hook.finishes && f.actual().reads == before.reads);
        CHECK(f.actual().state == block::DeviceState::quarantined); f.quiet(); ++cases;
    }
    {
        Fixture f; Hook hook; f.disk.media = Status::io_error("physical media failure");
        const auto before = f.actual(), decoy = f.decoy_report();
        f.disk.during_read = [&] { f.authority.enabled = false; f.authority.attached = f.decoy.get(); };
        hook.finish_status = Status::denied("external finish refusal");
        CHECK(guarded_ext4_direct_read(f.session(), f.shadow(), f.authority, 3, std::span{f.bytes}.first(1024), hook).code() == StatusCode::io_error);
        CHECK(f.actual().first_error == StatusCode::io_error && reclaim_history::same_session(f.shadow(), before));
        CHECK(reclaim_history::same_session(f.decoy_report(), decoy)); f.quiet(); ++cases;
    }
}
void nested_and_abandoned()
{
    {
        Fixture f; Status nested{};
        f.disk.during_read = [&] { nested = f.read(); };
        CHECK(!f.read() && !nested); CHECK(f.disk.reads == 1 && !f.shadow().reads);
        CHECK(f.actual().state == block::DeviceState::quarantined); f.quiet(); ++cases;
    }
    for (bool physical : {false, true})
    {
        Fixture f; const auto before = f.actual();
        {
            Scope scope(f.session(), f.shadow(), f.authority, 3, 1024); CHECK(scope.prepare());
            if (physical) CHECK(block::guarded_read_blocks(f.session(), scope, 3, std::span{f.bytes}.first(1024)));
            else CHECK(scope.begin_read(f.session(), 3, 1024));
        }
        CHECK(f.actual().state == block::DeviceState::quarantined && f.actual().first_error == StatusCode::interrupted);
        CHECK(f.disk.reads == usize(physical) && reclaim_history::same_session(f.shadow(), before)); f.quiet(); ++cases;
    }
}
void independent_retention_faults()
{
    for (usize selected = 0; selected < 8; ++selected)
    {
        Fixture f; const auto before = f.actual(), decoy = f.decoy_report();
        Scope scope(f.session(), f.shadow(), f.authority, 3, 1024); CHECK(scope.prepare());
        f.disk.during_read = [&] {
            switch (selected)
            {
            case 0: scope.original_ = &f.decoy->cache_.session_; break;
            case 1: scope.shadow_ = &f.decoy->reclaim_io_; break;
            case 2: scope.shadow_identity_ = &f.decoy->reclaim_io_; break;
            case 3: scope.anchor_.first_ = &f.decoy->cache_.session_; break;
            case 4: ++scope.before_.completed_flushes; break;
            case 5: ++scope.geometry_.block_count; break;
            case 6: scope.limit_ += 512; break;
            case 7: scope.begun_ = false; break;
            }
        };
        CHECK(block::guarded_read_blocks(f.session(), scope, 3, std::span{f.bytes}.first(1024)).code() == StatusCode::fault);
        const auto actual = f.actual();
        CHECK(actual.reads == before.reads + 1 && actual.completed_reads == before.completed_reads);
        CHECK(actual.state == block::DeviceState::quarantined && actual.first_error == StatusCode::fault);
        CHECK(reclaim_history::same_session(f.shadow(), before));
        CHECK(reclaim_history::same_session(f.decoy_report(), decoy) && reclaim_history::same_session(f.decoy->reclaim_io_, decoy));
        f.quiet(); ++cases;
    }
}
void shape_and_credit()
{
    for (const auto &request : std::array<std::pair<u64, usize>, 4>{{{3, 512}, {3, 1536}, {126, 1024}, {127, 512}}})
    {
        Fixture f; CHECK(f.read(request.first, request.second));
        CHECK(f.disk.reads == 1 && f.disk.sector == 37 + request.first && f.disk.size == request.second);
        CHECK(f.shadow().reads == 1 && f.shadow().completed_reads == 1); f.quiet(); ++cases;
    }
    for (const auto &request : std::array<std::pair<u64, usize>, 6>{{{3, 0}, {3, 513}, {3, 4096}, {128, 512}, {127, 1024}, {~u64{0}, 512}}})
    {
        Fixture f(2048); const auto before = f.actual(); CHECK(!f.read(request.first, request.second));
        CHECK(!f.disk.reads && reclaim_history::same_session(f.actual(), before) && reclaim_history::same_session(f.shadow(), before));
        CHECK(f.read()); f.quiet(); ++cases;
    }
    {
        Fixture f; auto &report = f.entry().report;
        report.reads = report.completed_reads = ~usize{0} - 1;
        report.writes = report.completed_writes = report.flushes = report.completed_flushes = ~usize{0};
        f.shadow() = report; CHECK(f.read());
        CHECK(f.disk.reads == 1 && f.shadow().reads == ~usize{0} && f.shadow().completed_reads == ~usize{0});
        CHECK(f.shadow().writes == ~usize{0} && f.shadow().flushes == ~usize{0}); f.quiet(); ++cases;
    }
    {
        Fixture f; auto &report = f.entry().report; report.reads = report.completed_reads = ~usize{0}; f.shadow() = report;
        const auto before = f.actual(); CHECK(f.read().code() == StatusCode::overflow);
        CHECK(!f.disk.reads && reclaim_history::same_session(f.actual(), before) && reclaim_history::same_session(f.shadow(), before));
        f.quiet(); ++cases;
    }
}
void geometry_boundaries()
{
    for (bool after : {false, true})
        for (bool attachment : {false, true})
        {
            Fixture f; const auto before = f.actual(), decoy = f.decoy_report(); bool introduced{};
            f.disk.during_geometry = [&] {
                const auto actual = f.actual();
                if (!introduced && actual.in_flight && f.disk.reads == usize(after))
                {
                    auto pending = before; ++pending.reads; pending.in_flight = true;
                    CHECK(reclaim_history::same_session(actual, pending));
                    CHECK(reclaim_history::same_session(f.shadow(), before));
                    introduced = true;
                    if (attachment) f.authority.attached = f.decoy.get();
                    else ++f.shadow().completed_flushes;
                }
            };
            // The default Hook has no in-flight method. The Scope must still
            // reach each Session boundary through the combined dispatcher.
            CHECK(f.read().code() == StatusCode::fault && introduced);
            f.disk.during_geometry = {};
            auto expected = before; ++expected.reads;
            expected.state = block::DeviceState::quarantined; expected.first_error = StatusCode::fault;
            CHECK(reclaim_history::same_session(f.actual(), expected));
            CHECK(f.disk.reads == usize(after));
            auto paused = before; if (!attachment) ++paused.completed_flushes;
            CHECK(reclaim_history::same_session(f.shadow(), paused));
            CHECK(reclaim_history::same_session(f.decoy_report(), decoy));
            for (usize index = 0; index < f.bytes.size(); ++index)
                CHECK(f.bytes[index] == (after && index < 1024 ? std::byte{0x35} : std::byte{}));
            CHECK(!f.read() && f.disk.reads == usize(after));
            f.quiet(); ++cases; ++geometry_cases;
        }
}
void external_in_flight_boundaries()
{
    for (usize selected = 0; selected < 3; ++selected)
        for (usize boundary = 1; boundary <= 4; ++boundary)
        {
            Fixture f; FlightHook hook; const auto before = f.actual(), decoy = f.decoy_report();
            hook.refusal = selected == 2 ? boundary : 0;
            hook.at_observation = [&](Session &session, usize observation) {
                CHECK(&session == &f.session());
                auto pending = before; ++pending.reads; pending.in_flight = true;
                CHECK(reclaim_history::same_session(f.actual(), pending));
                CHECK(reclaim_history::same_session(f.shadow(), before));
                CHECK(f.disk.reads == usize(observation >= 3));
                if (observation == boundary)
                {
                    if (selected == 0) f.authority.attached = f.decoy.get();
                    if (selected == 1) ++f.shadow().completed_flushes;
                }
            };
            const auto result = guarded_ext4_direct_read(f.session(), f.shadow(), f.authority,
                3, std::span{f.bytes}.first(1024), hook);
            const auto code = selected == 2 ? StatusCode::denied : StatusCode::fault;
            CHECK(result.code() == code && hook.observations == boundary);
            CHECK(hook.outer.begins == 1 && hook.outer.finishes == 1 && hook.outer.observed.code() == code);
            auto expected = before; ++expected.reads;
            expected.state = block::DeviceState::quarantined; expected.first_error = code;
            CHECK(reclaim_history::same_session(f.actual(), expected));
            CHECK(f.disk.reads == usize(boundary >= 3));
            auto paused = before; if (selected == 1) ++paused.completed_flushes;
            CHECK(reclaim_history::same_session(f.shadow(), paused));
            CHECK(reclaim_history::same_session(f.decoy_report(), decoy));
            const auto observations = hook.observations;
            CHECK(!guarded_ext4_direct_read(f.session(), f.shadow(), f.authority,
                3, std::span{f.bytes}.first(1024), hook));
            CHECK(hook.observations == observations && hook.outer.begins == 1 && hook.outer.finishes == 1);
            f.quiet(); ++cases; ++flight_cases;
        }
    {
        Fixture f; FlightHook hook; const auto before = f.actual();
        hook.at_observation = [&](Session &session, usize observation) {
            CHECK(&session == &f.session() && observation <= 4);
            auto pending = before; ++pending.reads; pending.in_flight = true;
            CHECK(reclaim_history::same_session(f.actual(), pending));
            CHECK(reclaim_history::same_session(f.shadow(), before));
        };
        CHECK(guarded_ext4_direct_read(f.session(), f.shadow(), f.authority,
            3, std::span{f.bytes}.first(1024), hook));
        CHECK(hook.observations == 4 && hook.outer.begins == 1 && hook.outer.finishes == 1);
        auto expected = before; ++expected.reads; ++expected.completed_reads;
        CHECK(reclaim_history::same_session(f.actual(), expected) && reclaim_history::same_session(f.shadow(), expected));
        CHECK(f.disk.reads == 1); f.quiet(); ++cases; ++flight_cases;
    }
    for (bool media_failure : {false, true})
    {
        Fixture f; FlightHook hook; const auto before = f.actual(), decoy = f.decoy_report();
        if (media_failure)
        {
            f.disk.media = Status::io_error("physical failure retains priority");
            f.disk.during_read = [&] { f.authority.attached = f.decoy.get(); };
            hook.outer.finish_status = Status::denied("external finish refusal after failed media");
        }
        else hook.outer.at_finish = [&] { f.authority.attached = f.decoy.get(); };
        const auto result = guarded_ext4_direct_read(f.session(), f.shadow(), f.authority,
            3, std::span{f.bytes}.first(1024), hook);
        const auto code = media_failure ? StatusCode::io_error : StatusCode::fault;
        CHECK(result.code() == code && hook.observations == (media_failure ? 2U : 4U));
        CHECK(hook.outer.begins == 1 && hook.outer.finishes == 1);
        CHECK(hook.outer.observed.code() == (media_failure ? StatusCode::io_error : StatusCode::ok));
        auto expected = before; ++expected.reads; expected.completed_reads += usize(!media_failure);
        expected.state = block::DeviceState::quarantined; expected.first_error = code;
        CHECK(reclaim_history::same_session(f.actual(), expected) && reclaim_history::same_session(f.shadow(), before));
        CHECK(reclaim_history::same_session(f.decoy_report(), decoy) && f.disk.reads == 1);
        f.quiet(); ++cases; ++flight_cases;
    }
}
void completed_pending_write_allowance()
{
    const auto establish = [](Fixture &f) {
        f.disk.write_media = {};
        CHECK(f.session().write_blocks(4, std::as_bytes(std::span{f.bytes}).first(1024)));
        f.shadow() = f.actual();
        CHECK(f.shadow().pending_writes && f.shadow().writes == 1 && f.shadow().completed_writes == 1);
    };
    const auto read = [](Fixture &f, FlightHook &hook) -> Status {
        Ext4DirectReadScope<Session, Authority, true> scope(f.session(), f.shadow(), f.authority, 3, 1024);
        if (auto status = scope.prepare(); !status) return status;
        Ext4DirectReadDispatch<decltype(scope), FlightHook, Session> dispatch{scope, hook};
        if (auto status = block::guarded_read_blocks(f.session(), dispatch, 3, std::span{f.bytes}.first(1024)); !status)
            return status;
        return scope.adopt(f.session());
    };
    for (usize selected = 0; selected < 6; ++selected)
    {
        Fixture f; establish(f); FlightHook hook;
        if (selected == 2) ++f.entry().report.writes;
        if (selected == 3) ++f.entry().report.completed_writes;
        if (selected == 4) ++f.entry().report.flushes;
        if (selected == 5) ++f.entry().report.completed_flushes;
        f.shadow() = f.actual(); const auto before = f.actual(), decoy = f.decoy_report();
        const auto result = selected == 1 ? f.read() : read(f, hook);
        auto expected = before;
        if (selected == 0)
        {
            CHECK(result && hook.observations == 4 && hook.outer.begins == 1 && hook.outer.finishes == 1);
            ++expected.reads; ++expected.completed_reads;
            CHECK(reclaim_history::same_session(f.shadow(), expected));
        }
        else
        {
            CHECK(result.code() == StatusCode::fault && !hook.observations && !hook.outer.begins && !hook.outer.finishes);
            expected.state = block::DeviceState::quarantined; expected.first_error = StatusCode::fault;
            CHECK(reclaim_history::same_session(f.shadow(), before));
        }
        CHECK(reclaim_history::same_session(f.actual(), expected));
        CHECK(f.disk.reads == usize(selected == 0) && f.disk.writes == 1 && !f.disk.flushes);
        CHECK(reclaim_history::same_session(f.decoy_report(), decoy));
        CHECK(!f.decoy_disk.reads && !f.decoy_disk.writes && !f.decoy_disk.flushes);
        ++cases; ++pending_cases;
    }
    for (bool after : {false, true})
        for (bool actual_bit : {false, true})
        {
            Fixture f; establish(f); FlightHook hook; const auto before = f.actual(), decoy = f.decoy_report(); bool changed{};
            f.disk.during_geometry = [&] {
                if (!changed && f.actual().in_flight && f.disk.reads == usize(after))
                {
                    changed = true;
                    if (actual_bit) f.entry().report.pending_writes = false;
                    else f.shadow().pending_writes = false;
                }
            };
            CHECK(read(f, hook).code() == StatusCode::fault && changed); f.disk.during_geometry = {};
            auto expected = before; ++expected.reads;
            expected.state = block::DeviceState::quarantined; expected.first_error = StatusCode::fault;
            if (actual_bit) expected.pending_writes = false;
            CHECK(reclaim_history::same_session(f.actual(), expected));
            auto paused = before; if (!actual_bit) paused.pending_writes = false;
            CHECK(reclaim_history::same_session(f.shadow(), paused));
            CHECK(f.disk.reads == usize(after) && f.disk.writes == 1 && !f.disk.flushes);
            CHECK(reclaim_history::same_session(f.decoy_report(), decoy));
            CHECK(!f.decoy_disk.reads && !f.decoy_disk.writes && !f.decoy_disk.flushes);
            ++cases; ++pending_cases;
        }
}
} // namespace
int main()
{
    successful_paused_prefix(); refusals(); shadow_and_attachment_drift(); nested_and_abandoned(); independent_retention_faults(); shape_and_credit();
    geometry_boundaries(); external_in_flight_boundaries(); completed_pending_write_allowance();
    std::printf("PASS direct read scope: checks=%zu cases=%zu geometry=%zu external_flight=%zu pending_allowance=%zu real Owner/Session exact attempted/completed prefixes; scoped READs add zero writes/flushes\n",
        checks, cases, geometry_cases, flight_cases, pending_cases);
}
