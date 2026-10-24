// Hosted actual five leaf paths, real prepared Owners and Sessions. Private
// access is solely for split startup setup and serialized negative injection.
// Compile with -fno-access-control. This is not guest or writer admission evidence.
#include "ext4_mounted_support.hpp"
#include "ext4_namespace_journal_fixture.hpp"
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_mounted_metadata.hpp"
#include "ok/fs/ext4_empty_orphan_recovery.hpp"
#include <memory>
#include <type_traits>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
#undef CHECK
namespace
{
usize checks{}, cases{}, leaf_cases[5]{}, geometry_cases{}, flight_cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "direct read leaf:%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
void checked(Status status, const char *label)
{
    ++checks;
    if (!status) { std::fprintf(stderr, "direct read leaf %s: %.*s\n", label, int(status.message().size()), status.message().data()); std::abort(); }
}
using Owner = Ext4OwnedTransaction<2, 8>;
using Registry = Owner::Registry;
using Session = Registry::Session;
using Mount = Ext4MountedMetadata<2>;
using Recovery = Ext4EmptyOrphanRecoveryOwner<2>;
using Lifetime = Ext4InodeLifetimeTable<128, 512>;
using Block = Ext4OrphanBlock;
enum class Leaf : usize { live, mounted_compare, standalone_compare, terminal, startup_read };
struct Hook final
{
    usize begins{}, finishes{};
    Status begin_status{}, finish_status{}, transport{};
    std::function<void()> at_begin{}, at_finish{};
    Status begin_read(Session &, u64, usize) { ++begins; if (at_begin) at_begin(); return begin_status; }
    Status finish_read(Session &, Status media) { ++finishes; transport = media; if (at_finish) at_finish(); return finish_status; }
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
        return observations == refusal ? Status::denied("leaf external in-flight refusal") : Status{};
    }
};
struct CallbackDisk final : driver::BlockDevice
{
    explicit CallbackDisk(Disk &disk) : backing(disk) {}
    driver::BlockGeometry geometry() const override
    { if (at_geometry) at_geometry(); return backing.geometry(); }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override { return backing.read_blocks(first, bytes); }
    Status write_blocks(u64 first, std::span<const std::byte> bytes) override { return backing.write_blocks(first, bytes); }
    Status flush() override { return backing.flush(); }
    Disk &backing;
    mutable std::function<void()> at_geometry{};
};
Bytes load(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes bytes((std::istreambuf_iterator<char>(input)), {}); CHECK(bytes.size() == 32 * 1024 * 1024); return bytes;
}
Block block(const Bytes &image, u32 physical)
{
    Block result{}; std::copy_n(image.begin() + physical * 1024, 1024, result.begin()); return result;
}

struct Live final
{
    Disk disk;
    CallbackDisk device{disk};
    Registry registry{};
    std::unique_ptr<Owner> owner{std::make_unique<Owner>(true)};
    Lifetime lifetime{};
    Lifetime::Ticket ticket{};
    Ext4InodeLifetimeKey key{};
    std::unique_ptr<Ext4OrphanChainCaptureStorage> storage{std::make_unique<Ext4OrphanChainCaptureStorage>()};
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch{std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>()};
    std::unique_ptr<Ext4OrphanReclaimCaptureStorage> observed{std::make_unique<Ext4OrphanReclaimCaptureStorage>()};
    std::unique_ptr<Ext4OrphanReclaimColdScratch> verification{std::make_unique<Ext4OrphanReclaimColdScratch>()};
    explicit Live(const Bytes &image) : disk(image)
    {
        disk.origin = 37;
        fixture::w32(disk.live, 1120, fixture::r32(disk.live, 1120) | 4U); fixture::super_crc(disk.live); disk.durable = disk.live;
        const auto registration = registry.register_device(device, 35101); CHECK(registration);
        checked(owner->open(registry, registration.value(), {37, 65536}), "live open");
        Ext4RecoveryBinding binding; Disk inspected(disk.live);
        checked(binding.inspect(inspected, {0, 65536}), "live binding");
        namespace_journal_model::Fixture source(disk.live, *binding.info(), 14, 15);
        const auto update = source.unlink("empty-note");
        checked(owner->prepare_namespace(update), "live unlink prepare");
        checked(owner->commit(), "live unlink commit"); checked(owner->checkpoint(), "live unlink checkpoint");
        const auto cache = owner->buffers(); key = {owner.get(), cache.session_serial, update.source, update.source_generation};
        checked(lifetime.observe(key), "live lifetime observe"); checked(lifetime.mark_orphan(key), "live lifetime orphan");
        checked(lifetime.schedule_exact(key, [&](const Lifetime::Ticket &issued) -> Status { ticket = issued; return {}; }), "live lifetime dispatch");
        checked(owner->prepare_orphan_reclaim(lifetime, ticket, key, *storage, *scratch, 1700000444, 123456789), "live reclaim prepare");
        checked(owner->publish_orphan_reclaim(*observed, *verification), "live durable publish");
        checked(owner->reclaim_guard(), "live continuing guard");
    }
    Owner &canonical() { return *owner; }
    template <typename ReadHook>
    Status call(ReadHook &hook, u64 first, std::span<std::byte>, const Block &expected)
    { return owner->reclaim_compare(u32(first / 2), expected, hook); }
    void drift() { owner->reclaim_storage_ = nullptr; }
};
struct Standalone final
{
    Disk disk;
    CallbackDisk device{disk};
    Registry registry{};
    std::unique_ptr<Ext4EmptyOrphanRecoveryWorkspace> workspace{std::make_unique<Ext4EmptyOrphanRecoveryWorkspace>()};
    std::unique_ptr<Recovery> recovery{std::make_unique<Recovery>()};
    typename Recovery::TerminalView terminal{*recovery};
    Leaf leaf;
    explicit Standalone(const Bytes &image, Leaf selected) : disk(image), leaf(selected)
    {
        disk.origin = 37;
        const auto registration = registry.register_device(device, 35102); CHECK(registration);
        checked(recovery->open(registry, registration.value(), {37, 65536}, *workspace), "standalone open");
        checked(recovery->prepare(0, 0), "standalone prepare"); checked(recovery->guard(), "standalone continuing guard");
    }
    Owner &canonical() { return recovery->owner_; }
    template <typename ReadHook>
    Status call(ReadHook &hook, u64 first, std::span<std::byte> out, const Block &expected)
    {
        return leaf == Leaf::terminal ? terminal.read_blocks_guarded(first, out, hook) : recovery->compare(u32(first / 2), expected, hook);
    }
    void drift() { recovery->seal_.canonical_owner = nullptr; }
};
struct Mounted final
{
    Disk disk;
    CallbackDisk device{disk};
    Registry registry{};
    std::unique_ptr<Ext4MountedStartupWorkspace> workspace{std::make_unique<Ext4MountedStartupWorkspace>()};
    std::unique_ptr<Mount> mount{std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get())};
    Leaf leaf;
    explicit Mounted(const Bytes &image, Leaf selected, bool opening = false) : disk(image), leaf(selected)
    {
        disk.origin = 37;
        const auto registration = registry.register_device(device, 35103); CHECK(registration);
        auto &owner = canonical();
        // Split the production recover_startup attachment from its synchronous
        // loop so the actual classifier and prepare stop before writer phases.
        mount->report_.phase = Ext4MountPhase::opening;
        checked(workspace->claim(mount.get(), &owner), "mounted permanent claim");
        checked(owner.open(registry, registration.value(), {37, 65536}), "mounted open");
        mount->identity_ = owner.buffers();
        owner.startup_workspace_ = workspace.get(); owner.startup_mount_ = mount.get(); owner.startup_epoch_ = 1;
        owner.startup_reader_ = &mount->startup_reader_; owner.startup_ordinary_reader_ = &mount->startup_ordinary_reader_;
        owner.startup_identity_ = owner.buffers(); owner.reclaim_io_ = owner.cache_.session_.report().value(); owner.publication_domain_ = 1;
        owner.startup_active_ = true; owner.startup_generation_ = 1; owner.report_.phase = Ext4OwnedPhase::preparing;
        owner.startup_report_.phase = Ext4StartupPhase::classifying;
        owner.startup_report_.original = owner.startup_report_.observed = owner.reclaim_io_;
        checked(owner.startup_domain_guard(), "mounted opening domain");
        checked(owner.startup_route_budget_.reserve_opening(owner.cache_.session_), "mounted opening reservation");
        owner.startup_route_opening_ = true;
        if (!opening)
        {
            checked(owner.startup_classify(), "mounted actual classify");
            CHECK(!owner.startup_route_opening_ && owner.startup_route_budget_.report().phase == Ext4ReclaimRoutePhase::closed);
            owner.startup_report_.phase = Ext4StartupPhase::draining;
            checked(owner.startup_prepare(0, 0), "mounted actual prepare"); checked(owner.startup_guard(), "mounted continuing guard");
        }
    }
    Owner &canonical() { return mount->owner_; }
    template <typename ReadHook>
    Status call(ReadHook &hook, u64 first, std::span<std::byte> out, const Block &expected)
    {
        auto &owner = canonical();
        return leaf == Leaf::startup_read ? owner.startup_read_blocks(first, out, hook) : owner.startup_compare(u32(first / 2), expected, hook);
    }
    void drift() { canonical().startup_seal_.active = false; }
};

enum class Scenario { success, begin_refusal, finish_refusal, media_and_finish_refusal, media_authority_drift,
                      shadow_drift, attachment_drift, finish_shadow_drift, finish_attachment_drift, reentry, mismatch };
template <typename Fixture>
void scenario(Fixture &f, Leaf leaf, Scenario selected)
{
    auto &owner = f.canonical(); auto &session = owner.cache_.session_;
    const auto before = session.report().value(); CHECK(reclaim_history::same_session(before, owner.reclaim_io_));
    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
    Disk decoy_disk(f.disk.live); decoy_disk.origin = 83; Session decoy;
    const auto decoy_key = f.registry.register_device(decoy_disk, 35199); CHECK(decoy_key);
    checked(f.registry.acquire(decoy_key.value(), {83, 65536}, block::SessionAccess::writable, decoy), "decoy acquire");
    const auto decoy_before = decoy.report().value();
    Hook hook; usize observed{}; Status nested{};
    Block expected = block(f.disk.live, 1);
    std::array<std::byte, 1024> bytes{};
    if (selected == Scenario::mismatch) expected[19] ^= 1;
    if (selected == Scenario::begin_refusal) hook.begin_status = Status::denied("external leaf begin refusal");
    if (selected == Scenario::finish_refusal || selected == Scenario::media_and_finish_refusal)
        hook.finish_status = Status::denied("external leaf finish refusal");
    if (selected == Scenario::media_and_finish_refusal || selected == Scenario::media_authority_drift)
        f.disk.fail_read = reads + 1;
    if (selected == Scenario::finish_shadow_drift) hook.at_finish = [&] { ++owner.reclaim_io_.completed_flushes; };
    if (selected == Scenario::finish_attachment_drift) hook.at_finish = [&] { f.drift(); };
    const auto outside_finish = hook.at_finish;
    hook.at_finish = [&] {
        const auto actual = session.report().value();
        CHECK(actual.reads == before.reads + 1 && !actual.in_flight);
        const bool completed = selected != Scenario::media_and_finish_refusal && selected != Scenario::media_authority_drift &&
            selected != Scenario::shadow_drift && selected != Scenario::attachment_drift;
        CHECK(actual.completed_reads == before.completed_reads + usize(completed));
        CHECK(owner.reclaim_io_.reads == before.reads && owner.reclaim_io_.completed_reads == before.completed_reads);
        if (outside_finish) outside_finish();
    };
    f.disk.hook = [&](char op) {
        CHECK(op == 'r'); ++observed;
        auto pending = before; ++pending.reads; pending.in_flight = true;
        CHECK(reclaim_history::same_session(session.report().value(), pending));
        CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
        if (selected == Scenario::shadow_drift) ++owner.reclaim_io_.completed_flushes;
        if (selected == Scenario::attachment_drift || selected == Scenario::media_authority_drift) f.drift();
        if (selected == Scenario::reentry) { Hook inner; nested = f.call(inner, 2, bytes, expected); CHECK(!nested && !inner.begins && !inner.finishes); }
    };
    const auto result = f.call(hook, 2, bytes, expected); f.disk.hook = {};
    const auto actual = session.report().value();
    const bool physical = selected != Scenario::begin_refusal;
    const bool completed = physical && selected != Scenario::media_and_finish_refusal && selected != Scenario::media_authority_drift &&
        selected != Scenario::shadow_drift && selected != Scenario::attachment_drift;
    CHECK(hook.begins == 1 && hook.finishes == usize(physical) && observed == usize(physical));
    CHECK(f.disk.reads == reads + usize(physical) && f.disk.writes == writes && f.disk.flushes == flushes);
    CHECK(actual.reads == before.reads + usize(physical) && actual.completed_reads == before.completed_reads + usize(completed));
    CHECK(reclaim_history::same_session(decoy.report().value(), decoy_before) && !decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes);
    if (selected == Scenario::success || selected == Scenario::reentry)
    {
        checked(result, "leaf complete dispatch"); CHECK(reclaim_history::same_session(actual, owner.reclaim_io_));
        CHECK(actual.state == block::DeviceState::owned && actual.first_error == StatusCode::ok);
    }
    else if (selected == Scenario::mismatch)
    {
        CHECK(leaf != Leaf::terminal && leaf != Leaf::startup_read && result.code() == StatusCode::io_error);
        CHECK(reclaim_history::same_session(actual, owner.reclaim_io_) && actual.state == block::DeviceState::owned);
    }
    else
    {
        CHECK(!result && owner.reclaim_io_.reads == before.reads && owner.reclaim_io_.completed_reads == before.completed_reads);
        if (!physical) CHECK(result.code() == StatusCode::denied && reclaim_history::same_session(actual, before));
        else
        {
            const auto code = selected == Scenario::finish_refusal ? StatusCode::denied :
                selected == Scenario::media_and_finish_refusal || selected == Scenario::media_authority_drift ? StatusCode::io_error : StatusCode::fault;
            CHECK(result.code() == code && actual.state == block::DeviceState::quarantined && actual.first_error == code);
            Hook refused; const auto again = f.call(refused, 2, bytes, expected);
            CHECK(!again && !refused.begins && !refused.finishes && f.disk.reads == reads + 1);
        }
    }
    ++cases; ++leaf_cases[usize(leaf)];
}
template <typename Create>
void sweep(Leaf leaf, Create create)
{
    for (const auto selected : {Scenario::success, Scenario::begin_refusal, Scenario::finish_refusal,
        Scenario::media_and_finish_refusal, Scenario::media_authority_drift, Scenario::shadow_drift, Scenario::attachment_drift,
        Scenario::finish_shadow_drift, Scenario::finish_attachment_drift, Scenario::reentry})
    { auto fixture = create(); scenario(*fixture, leaf, selected); }
    if (leaf == Leaf::live || leaf == Leaf::mounted_compare || leaf == Leaf::standalone_compare)
    { auto fixture = create(); scenario(*fixture, leaf, Scenario::mismatch); }
}

template <typename Create>
void credit_sweep(Leaf leaf, Create create)
{
    for (usize selected = 0; selected < 3; ++selected)
    {
        auto f = create(); auto &owner = f->canonical(); auto &session = owner.cache_.session_;
        auto &report = f->registry.entries_[session.registration_.slot_].report;
        report.reads = report.completed_reads = ~usize{0} - usize(selected != 1);
        if (selected == 2) report.completed_reads = ~usize{0};
        report.writes = report.completed_writes = report.flushes = report.completed_flushes = ~usize{0};
        owner.reclaim_io_ = report; const auto before = report;
        const auto reads = f->disk.reads, writes = f->disk.writes, flushes = f->disk.flushes;
        std::array<std::byte, 1024> bytes{}; Hook hook;
        const auto result = f->call(hook, 2, bytes, block(f->disk.live, 1));
        const auto actual = session.report().value();
        CHECK(f->disk.writes == writes && f->disk.flushes == flushes);
        if (selected == 0)
        {
            checked(result, "leaf final read credit with exhausted unrelated counters");
            CHECK(hook.begins == 1 && hook.finishes == 1 && f->disk.reads == reads + 1);
            CHECK(actual.reads == ~usize{0} && actual.completed_reads == ~usize{0});
            CHECK(reclaim_history::same_session(actual, owner.reclaim_io_));
        }
        else
        {
            CHECK(!result && !hook.begins && !hook.finishes && f->disk.reads == reads);
            CHECK(result.code() == (selected == 1 ? StatusCode::overflow : StatusCode::fault));
            CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
            CHECK(actual.reads == before.reads && actual.completed_reads == before.completed_reads);
        }
        ++cases; ++leaf_cases[usize(leaf)];
    }
}

void terminal_shapes(const Bytes &image)
{
    for (const auto &request : std::array<std::pair<u64, usize>, 4>{{{3, 512}, {3, 1536}, {65534, 1024}, {65535, 512}}})
    {
        auto f = std::make_unique<Standalone>(image, Leaf::terminal); auto &owner = f->canonical();
        const auto before = owner.reclaim_io_; const auto reads = f->disk.reads, writes = f->disk.writes, flushes = f->disk.flushes;
        std::array<std::byte, 4096> bytes{}; Hook hook;
        checked(f->call(hook, request.first, std::span{bytes}.first(request.second), {}), "terminal shape");
        CHECK(hook.begins == 1 && hook.finishes == 1 && f->disk.reads == reads + 1);
        CHECK(f->disk.trace.back().sector == 37 + request.first && f->disk.trace.back().size == request.second);
        CHECK(owner.reclaim_io_.reads == before.reads + 1 && owner.reclaim_io_.completed_reads == before.completed_reads + 1);
        CHECK(f->disk.writes == writes && f->disk.flushes == flushes); ++cases;
    }
    for (const auto &request : std::array<std::pair<u64, usize>, 7>{{{3, 0}, {3, 513}, {3, 66048}, {65536, 512}, {65535, 1024}, {~u64{0}, 512}, {3, 65537}}})
    {
        auto f = std::make_unique<Standalone>(image, Leaf::terminal); auto &owner = f->canonical();
        const auto before = owner.reclaim_io_; const auto reads = f->disk.reads;
        std::array<std::byte, 66048> bytes{}; Hook hook;
        CHECK(!f->call(hook, request.first, std::span{bytes}.first(request.second), {}));
        CHECK(!hook.finishes && f->disk.reads == reads && reclaim_history::same_session(owner.reclaim_io_, before));
        CHECK(reclaim_history::same_session(owner.cache_.session_.report().value(), before)); ++cases;
    }
}
void startup_opening_branch(const Bytes &image)
{
    auto f = std::make_unique<Mounted>(image, Leaf::startup_read, true); auto &owner = f->canonical();
    std::array<std::byte, 1024> bytes{}; Hook hook; const auto before = owner.reclaim_io_;
    const auto reserved = owner.startup_route_budget_.report();
    checked(f->call(hook, 2, bytes, {}), "original opening Budget read");
    CHECK(!hook.begins && !hook.finishes && owner.startup_route_opening_);
    CHECK(owner.startup_route_budget_.report().attempts[0] == reserved.attempts[0] + 1);
    CHECK(owner.reclaim_io_.reads == before.reads + 1 && reclaim_history::same_session(owner.reclaim_io_, owner.cache_.session_.report().value()));
    checked(owner.startup_close_opening_route(), "opening close");
    const auto closed = owner.startup_route_budget_.report(); CHECK(closed.phase == Ext4ReclaimRoutePhase::closed);
    checked(f->call(hook, 2, bytes, {}), "post-close direct read");
    CHECK(hook.begins == 1 && hook.finishes == 1 && !owner.startup_route_opening_);
    CHECK(owner.startup_route_budget_.report().attempts == closed.attempts && owner.startup_route_budget_.report().phase == Ext4ReclaimRoutePhase::closed);
    CHECK(!f->disk.writes && !f->disk.flushes); ++cases;
}
bool same_budget(const Ext4ReclaimRouteBudgetReport &a, const Ext4ReclaimRouteBudgetReport &b)
{
    return a.phase == b.phase && a.capacity.reads == b.capacity.reads && a.capacity.writes == b.capacity.writes &&
        a.capacity.flushes == b.capacity.flushes && a.capacity.remaining == b.capacity.remaining &&
        a.capacity.projected_sequence_end == b.capacity.projected_sequence_end && a.attempts == b.attempts &&
        a.completions == b.completions && reclaim_history::same_session(a.observed, b.observed) &&
        a.attempted == b.attempted && a.refined == b.refined && a.dispatch_pending == b.dispatch_pending;
}
void startup_selected_attachment_faults(const Bytes &image)
{
    for (usize selected = 0; selected < 3; ++selected)
    {
        auto f = std::make_unique<Mounted>(image, Leaf::startup_read); auto &owner = f->canonical();
        auto &session = owner.cache_.session_; const auto before = owner.reclaim_io_;
        const auto budget = owner.startup_route_budget_.report(); CHECK(budget.phase == Ext4ReclaimRoutePhase::closed);
        CHECK(!owner.startup_route_opening_ && !owner.startup_namespace_seal_.active);
        const auto reads = f->disk.reads, writes = f->disk.writes, flushes = f->disk.flushes;
        Disk decoy_disk(f->disk.live); decoy_disk.origin = 83; Session decoy;
        const auto key = f->registry.register_device(decoy_disk, 35299); CHECK(key);
        checked(f->registry.acquire(key.value(), {83, 65536}, block::SessionAccess::writable, decoy), "startup mutation decoy");
        const auto decoy_before = decoy.report().value();
        std::array<std::byte, 1024> bytes{}; Hook hook; usize observations{};
        f->disk.hook = [&](char op) {
            CHECK(op == 'r'); ++observations;
            auto pending = before; ++pending.reads; pending.in_flight = true;
            CHECK(reclaim_history::same_session(session.report().value(), pending));
            CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
            if (selected == 0) owner.startup_route_opening_ = true;
            if (selected == 1) owner.startup_namespace_seal_.active = true;
        };
        if (selected == 2) hook.at_finish = [&] {
            auto completed = before; ++completed.reads; ++completed.completed_reads;
            CHECK(reclaim_history::same_session(session.report().value(), completed));
            CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
            owner.startup_namespace_seal_.active = true;
        };
        CHECK(f->call(hook, 2, bytes, {}).code() == StatusCode::fault); f->disk.hook = {};
        const auto actual = session.report().value();
        CHECK(actual.state == block::DeviceState::quarantined && actual.first_error == StatusCode::fault);
        CHECK(actual.reads == before.reads + 1 && actual.completed_reads == before.completed_reads + usize(selected == 2) && !actual.in_flight);
        CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
        CHECK(hook.begins == 1 && hook.finishes == 1 && observations == 1 && f->disk.reads == reads + 1);
        CHECK(f->disk.writes == writes && f->disk.flushes == flushes);
        CHECK(same_budget(owner.startup_route_budget_.report(), budget));
        CHECK(reclaim_history::same_session(decoy.report().value(), decoy_before));
        CHECK(!decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes);
        ++cases; ++leaf_cases[usize(Leaf::startup_read)];
    }
}
void startup_request_shapes(const Bytes &image)
{
    for (const auto &request : std::array<std::pair<u64, usize>, 6>{{{65534, 1024}, {3, 1024}, {2, 512}, {2, 0}, {65536, 1024}, {~u64{0}, 1024}}})
    {
        auto f = std::make_unique<Mounted>(image, Leaf::startup_read); auto &owner = f->canonical();
        const auto before = owner.reclaim_io_; const auto budget = owner.startup_route_budget_.report();
        CHECK(!owner.startup_route_opening_ && budget.phase == Ext4ReclaimRoutePhase::closed);
        const auto reads = f->disk.reads, writes = f->disk.writes, flushes = f->disk.flushes;
        std::array<std::byte, 1024> bytes{}; Hook hook;
        const auto result = f->call(hook, request.first, std::span{bytes}.first(request.second), {});
        if (request.first == 65534)
        {
            checked(result, "startup last bounded whole-block read");
            CHECK(hook.begins == 1 && hook.finishes == 1 && f->disk.reads == reads + 1);
            CHECK(f->disk.trace.back().sector == 37 + request.first && f->disk.trace.back().size == 1024);
            auto completed = before; ++completed.reads; ++completed.completed_reads;
            CHECK(reclaim_history::same_session(owner.reclaim_io_, completed));
            CHECK(reclaim_history::same_session(owner.cache_.session_.report().value(), completed));
        }
        else
        {
            CHECK(result.code() == StatusCode::overflow && !hook.begins && !hook.finishes && f->disk.reads == reads);
            CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
            CHECK(reclaim_history::same_session(owner.cache_.session_.report().value(), before));
        }
        CHECK(f->disk.writes == writes && f->disk.flushes == flushes);
        CHECK(!owner.startup_route_opening_ && same_budget(owner.startup_route_budget_.report(), budget));
        ++cases; ++leaf_cases[usize(Leaf::startup_read)];
    }
}
template <typename Create>
void boundary_sweep(Leaf leaf, Create create)
{
    for (usize selected = 0; selected < 17; ++selected)
    {
        auto f = create(); auto &owner = f->canonical(); auto &session = owner.cache_.session_;
        const auto before = session.report().value();
        CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
        const auto reads = f->disk.reads, writes = f->disk.writes, flushes = f->disk.flushes;
        const auto media = f->disk.live, durable = f->disk.durable;
        Disk decoy_disk(media); decoy_disk.origin = 83; Session decoy;
        const auto key = f->registry.register_device(decoy_disk, 35399); CHECK(key);
        checked(f->registry.acquire(key.value(), {83, 65536}, block::SessionAccess::writable, decoy), "boundary decoy");
        const auto decoy_before = decoy.report().value();
        std::array<std::byte, 1024> bytes{}; Hook plain; FlightHook flight; bool introduced{};
        usize physical{}; StatusCode code = StatusCode::fault;
        if (selected < 4)
        {
            const bool after = selected & 1, attachment = selected & 2;
            physical = usize(after);
            f->device.at_geometry = [&, attachment] {
                if (!introduced && session.report().value().in_flight && f->disk.reads == reads + physical)
                {
                    introduced = true;
                    if (attachment) f->drift(); else ++owner.reclaim_io_.completed_flushes;
                }
            };
        }
        else
        {
            const usize boundary = (selected - 4) % 4 + 1, mutation = (selected - 4) / 4;
            physical = usize(selected == 16 || boundary >= 3);
            code = selected == 16 ? StatusCode::ok : mutation == 2 ? StatusCode::denied : StatusCode::fault;
            flight.refusal = mutation == 2 ? boundary : 0;
            flight.at_observation = [&, boundary, mutation](Session &observed, usize observation) {
                CHECK(&observed == &session && observation <= 4);
                auto pending = before; ++pending.reads; pending.in_flight = true;
                CHECK(reclaim_history::same_session(session.report().value(), pending));
                CHECK(reclaim_history::same_session(owner.reclaim_io_, before));
                CHECK(f->disk.reads == reads + usize(observation >= 3));
                if (observation == boundary && selected != 16)
                {
                    introduced = true;
                    if (mutation == 0) f->drift();
                    if (mutation == 1) ++owner.reclaim_io_.completed_flushes;
                }
            };
        }
        const auto expected_block = block(media, 1);
        const auto result = selected < 4 ? f->call(plain, 2, bytes, expected_block) : f->call(flight, 2, bytes, expected_block);
        f->device.at_geometry = {};
        CHECK(result.code() == code && (selected == 16 || introduced));
        const auto actual = session.report().value();
        auto expected = before; ++expected.reads;
        if (selected == 16) ++expected.completed_reads;
        else { expected.state = block::DeviceState::quarantined; expected.first_error = code; }
        CHECK(reclaim_history::same_session(actual, expected));
        CHECK(f->disk.reads == reads + physical && f->disk.writes == writes && f->disk.flushes == flushes);
        CHECK(f->disk.live == media && f->disk.durable == durable);
        if (selected < 4)
        {
            CHECK(plain.begins == 1 && plain.finishes == 1 && plain.transport.code() == code);
            ++geometry_cases;
        }
        else
        {
            CHECK(flight.observations == (selected == 16 ? 4 : (selected - 4) % 4 + 1));
            CHECK(flight.outer.begins == 1 && flight.outer.finishes == 1 && flight.outer.transport.code() == code);
            ++flight_cases;
        }
        if (selected == 16) CHECK(reclaim_history::same_session(owner.reclaim_io_, actual));
        else
        {
            auto paused = before;
            if ((selected < 4 && !(selected & 2)) || (selected >= 8 && selected <= 11)) ++paused.completed_flushes;
            CHECK(reclaim_history::same_session(owner.reclaim_io_, paused));
            Hook again; CHECK(!f->call(again, 2, bytes, expected_block) && !again.begins && !again.finishes);
            CHECK(f->disk.reads == reads + physical);
        }
        CHECK(reclaim_history::same_session(decoy.report().value(), decoy_before));
        CHECK(!decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes);
        ++cases; ++leaf_cases[usize(leaf)];
    }
}
} // namespace
int main(int argc, char **argv)
{
    CHECK(argc == 3); const auto orphan = load(argv[1]), namespace_image = load(argv[2]);
    sweep(Leaf::live, [&] { return std::make_unique<Live>(namespace_image); });
    sweep(Leaf::mounted_compare, [&] { return std::make_unique<Mounted>(orphan, Leaf::mounted_compare); });
    sweep(Leaf::standalone_compare, [&] { return std::make_unique<Standalone>(orphan, Leaf::standalone_compare); });
    sweep(Leaf::terminal, [&] { return std::make_unique<Standalone>(orphan, Leaf::terminal); });
    sweep(Leaf::startup_read, [&] { return std::make_unique<Mounted>(orphan, Leaf::startup_read); });
    credit_sweep(Leaf::live, [&] { return std::make_unique<Live>(namespace_image); });
    credit_sweep(Leaf::mounted_compare, [&] { return std::make_unique<Mounted>(orphan, Leaf::mounted_compare); });
    credit_sweep(Leaf::standalone_compare, [&] { return std::make_unique<Standalone>(orphan, Leaf::standalone_compare); });
    credit_sweep(Leaf::terminal, [&] { return std::make_unique<Standalone>(orphan, Leaf::terminal); });
    credit_sweep(Leaf::startup_read, [&] { return std::make_unique<Mounted>(orphan, Leaf::startup_read); });
    terminal_shapes(orphan); startup_opening_branch(orphan);
    startup_selected_attachment_faults(orphan); startup_request_shapes(orphan);
    boundary_sweep(Leaf::live, [&] { return std::make_unique<Live>(namespace_image); });
    boundary_sweep(Leaf::mounted_compare, [&] { return std::make_unique<Mounted>(orphan, Leaf::mounted_compare); });
    boundary_sweep(Leaf::standalone_compare, [&] { return std::make_unique<Standalone>(orphan, Leaf::standalone_compare); });
    boundary_sweep(Leaf::terminal, [&] { return std::make_unique<Standalone>(orphan, Leaf::terminal); });
    boundary_sweep(Leaf::startup_read, [&] { return std::make_unique<Mounted>(orphan, Leaf::startup_read); });
    std::printf("PASS direct read leaves: checks=%zu cases=%zu leaves=%zu,%zu,%zu,%zu,%zu geometry=%zu external_flight=%zu actual prepare, paused shadow, exact completion boundary and both startup routes; zero read-scope writes/flushes\n",
        checks, cases, leaf_cases[0], leaf_cases[1], leaf_cases[2], leaf_cases[3], leaf_cases[4], geometry_cases, flight_cases);
}
