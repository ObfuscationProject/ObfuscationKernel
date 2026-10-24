// Actual hosted mounted provider/Session/cache and durable-write model.
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
static usize checks{}, callbacks{}, mounted{}, rejected{}, faults{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL startup:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
static void checked(Status status, const char *label)
{ ++checks; if (!status) { std::fprintf(stderr, "FAIL %s: %.*s\n", label, int(status.message().size()), status.message().data()); std::abort(); } }
using Mount = Ext4MountedMetadata<2>;
static Bytes load(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes result((std::istreambuf_iterator<char>(input)), {}); CHECK(result.size() == 32 * 1024 * 1024); return result;
}
struct Case
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Ext4MountedReclaimWorkspace> live = std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount;
    usize admission_reads{}, admission_writes{}, admission_flushes{};
    bool observed_admission{};
    explicit Case(const Bytes &image, bool supplied = true) : disk(image), mount(std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime, live.get(), supplied ? startup.get() : nullptr))
    {
        disk.origin = 11;
        auto registration = registry.register_device(disk, 49101, 65536); CHECK(registration); key = registration.value();
    }
    Status run() { return mount->mount(registry, key, {11, 65536}); }
    void observer()
    {
        disk.hook = [&](char) {
            const auto report = mount->report();
            const auto startup_report = mount->startup_report();
            if (startup_report.phase == Ext4StartupPhase::complete && !observed_admission)
            {
                // The terminal startup snapshot precedes ordinary Node construction.
                admission_reads = disk.reads; admission_writes = disk.writes; admission_flushes = disk.flushes;
                CHECK(startup_report.observed.reads == admission_reads && startup_report.observed.writes == admission_writes &&
                      startup_report.observed.flushes == admission_flushes);
                observed_admission = true;
            }
            CHECK(report.phase == Ext4MountPhase::opening && !report.published && !report.epoch);
            CHECK(!mount->root() && !mount->lifetime_snapshot(2));
            CHECK(mount->sync().code() == StatusCode::busy && mount->close().code() == StatusCode::busy &&
                  mount->freeze_clean().code() == StatusCode::busy);
            Mount::Registry::Session foreign;
            CHECK(registry.acquire(key, {11, 65536}, block::SessionAccess::readonly, foreign).code() == StatusCode::busy);
            ++callbacks;
        };
    }
    void failed()
    {
        CHECK(mount->report().phase == Ext4MountPhase::failed && !mount->root() && !mount->lifetime_snapshot(2));
        CHECK(registry.inspect(key).value().state == block::DeviceState::quarantined);
        const auto reads = disk.reads, writes = disk.writes, flushes = disk.flushes;
        CHECK(!mount->sync() && !mount->freeze_clean() && !mount->close() && !run());
        CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes);
        auto other = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, startup.get());
        CHECK(!other->mount(registry, key, {11, 65536}));
        CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes);
    }
};
static void success(const Bytes &input, const Bytes &golden, const std::filesystem::path &output,
                    const char *name, usize members)
{
    auto c = std::make_unique<Case>(input); c->observer(); testing::ext4_clock::reset();
    checked(c->run(), name); c->disk.hook = {};
    const auto report = c->mount->startup_report(); const auto io = c->registry.inspect(c->key).value();
    CHECK(report.phase == Ext4StartupPhase::complete && report.strict_ordinary_admission && report.root_unpublished);
    CHECK(report.initial_family == (members ? Ext4StartupFamily::empty_orphan : Ext4StartupFamily::ordinary));
    CHECK(report.chain_members == members && report.reclaimed == members && report.verified_publications == 3 * members);
    CHECK(!report.original.reads && !report.original.writes && !report.original.flushes && testing::ext4_clock::calls() == 0);
    CHECK(report.original.identity == report.observed.identity &&
          report.original.registration_generation == report.observed.registration_generation &&
          report.original.session_serial == report.observed.session_serial);
    CHECK(report.observed.reads == (c->observed_admission ? c->admission_reads : c->disk.reads) &&
          report.observed.writes == (c->observed_admission ? c->admission_writes : c->disk.writes) &&
          report.observed.flushes == (c->observed_admission ? c->admission_flushes : c->disk.flushes));
    CHECK(io.reads == c->disk.reads && io.writes == c->disk.writes && io.flushes == c->disk.flushes && io.completed_reads == io.reads &&
          io.completed_writes == io.writes && io.completed_flushes == io.flushes && !io.pending_writes && !io.in_flight);
    CHECK(c->disk.live == golden && c->disk.durable == golden && c->disk.pending.empty());
    CHECK(c->mount->report().phase == Ext4MountPhase::readable && c->mount->report().epoch == 1 && !c->mount->report().published);
    for (const auto &event : c->disk.trace)
        if (event.operation != 'f') CHECK(event.sector >= 11 && event.sector + event.size / 512 <= 65536 + 11);
    save(output / (std::string(name) + "-startup.img"), c->disk.durable);
    auto vfs = std::make_unique<VirtualFileSystem>();
    auto publication = publish_owned_root(*c->mount, *vfs); CHECK(publication);
    testing::ext4_clock::set({{1800000666, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software});
    const bool notes_profile = std::string_view{name} == "notes" || std::string_view{name} == "stale-padding";
    const auto lookup_file = [&](VirtualFileSystem &filesystem) -> Result<PathReference> {
        return filesystem.resolve({}, notes_profile ? "/home/notes.txt" : "/basic-note");
    };
    auto child = lookup_file(*vfs); checked(child.status(), "ordinary file lookup"); CHECK(child);
    const auto data = child.value().node()->read(0, 16); CHECK(data && data.value().size == 16);
    checked(child.value().node()->write(0, std::span{data.value().data}.first(16)), "first normal same-byte write");
    CHECK(c->mount->report().epoch == 2);
    checked(child.value().node()->write(0, std::span{data.value().data}.first(16)), "second normal same-byte write");
    CHECK(c->mount->report().epoch == 3);
    const auto after = child.value().node()->read(0, 16); CHECK(after && after.value().size == 16 && after.value().data == data.value().data);
    checked(c->mount->sync(), "same Session sync");
    CHECK(c->registry.inspect(c->key).value().session_serial == io.session_serial);
    checked(child.value().release(), "release ordinary file path");
    checked(detach_and_drain(*c->mount, *vfs, publication.value()), "ordinary detach and drain");
    checked(c->mount->freeze_clean(), "clean freeze");
    auto reopened = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    checked(reopened->mount(c->registry, c->key, {11, 65536}), "fresh ordinary reopen");
    auto reopened_vfs = std::make_unique<VirtualFileSystem>();
    auto reopened_publication = publish_owned_root(*reopened, *reopened_vfs); CHECK(reopened_publication);
    auto file = lookup_file(*reopened_vfs); checked(file.status(), "reopened file lookup"); CHECK(file);
    const auto persisted = file.value().node()->read(0, 16); CHECK(persisted && persisted.value().data == data.value().data);
    CHECK(c->registry.inspect(c->key).value().session_serial > io.session_serial);
    checked(file.value().release(), "release reopened file path");
    checked(detach_and_drain(*reopened, *reopened_vfs, reopened_publication.value()), "reopened detach and drain");
    checked(reopened->freeze_clean(), "reopened clean freeze");
    save(output / (std::string(name) + "-final.img"), c->disk.durable); ++mounted;
}
static void refusal(const Bytes &input, const char *label)
{
    auto c = std::make_unique<Case>(input); c->observer(); const auto status = c->run(); c->disk.hook = {};
    CHECK(!status && !c->disk.writes && !c->disk.flushes && c->disk.live == input && c->disk.durable == input);
    c->failed(); ++rejected;
    std::printf("STARTUP_REJECT_PASS case=%s zero_write_flush=1 reason=%.*s\n", label, int(status.message().size()), status.message().data());
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); const std::filesystem::path inputs = argv[1], output = argv[2]; std::filesystem::create_directory(output);
    const auto notes = load(inputs / "notes-empty-orphan.img");
    success(notes, load(inputs / "notes-finished.img"), output, "notes", 1);
    success(load(inputs / "input-empty.img"), load(inputs / "finished-empty.img"), output, "empty", 1);
    success(load(inputs / "input-data.img"), load(inputs / "finished-data.img"), output, "data", 1);
    const auto chain2 = load(inputs / "input-chain2-cross-group.img");
    success(chain2, load(inputs / "finished-chain2.img"), output, "chain2", 2);
    success(load(inputs / "input-chain8-cross-group.img"), load(inputs / "finished-chain8.img"), output, "chain8", 8);
    success(load(inputs / "finished-chain2.img"), load(inputs / "finished-chain2.img"), output, "ordinary", 0);
    auto stale = notes, stale_golden = load(inputs / "notes-finished.img");
    stale[journal(stale) + 100 * 1024] ^= 0x33; stale_golden[journal(stale_golden) + 100 * 1024] ^= 0x33;
    success(stale, stale_golden, output, "stale-padding", 1);
    auto bad = notes; bad[1024 + 1020] ^= 1; refusal(bad, "superblock-CRC");
    bad = notes; w32(bad, 1256, 9999); super_crc(bad); refusal(bad, "head-bounds");
    bad = notes; const auto at = inode_offset(bad, 20); w32(bad, at + 100, r32(bad, at + 100) + 1); refusal(bad, "generation-CRC");
    bad = notes; const u32 head = r32(bad, 1256); const auto head_at = inode_offset(bad, head);
    w32(bad, head_at + 100, ~u32{0}); inode_crc(bad, head, head_at); refusal(bad, "generation-capacity");
    bad = chain2; const u32 first = r32(bad, 1256), later = r32(bad, inode_offset(bad, first) + 20);
    CHECK(later); const auto later_at = inode_offset(bad, later);
    w32(bad, later_at + 100, ~u32{0}); inode_crc(bad, later, later_at); refusal(bad, "later-generation-capacity");
    bad = chain2; w32(bad, later_at + 132, 1); inode_crc(bad, later, later_at); refusal(bad, "later-ctime-profile");
    bad = notes; w32(bad, at + 12, 0x80000000); inode_crc(bad, 20, at); refusal(bad, "ctime-profile");
    bad = notes; w32(bad, at + 20, 20); inode_crc(bad, 20, at); refusal(bad, "chain-cycle");
    refusal(load(inputs / "chain9.img"), "chain-capacity");
    // Writable namespace positives now belong to ext4_namespace_result_history_tests.cpp;
    // this suite keeps the empty-orphan C1 regressions and zero-I/O refusals.
    for (const char *name : {"committed-before", "committed-prefix", "committed-after", "invalid-barrier", "corrupt-witness", "third-home"})
        refusal(load(inputs / (std::string("reclaim-") + name + ".img")), name);
    {
        auto c = std::make_unique<Case>(notes, false); CHECK(!c->run() && !c->disk.writes && !c->disk.flushes);
        CHECK(!c->mount->root()); ++rejected;
    }
    for (char kind : {'r', 'w', 'f'})
    {
        auto c = std::make_unique<Case>(notes); c->observer();
        if (kind == 'r') c->disk.fail_read = 4;
        if (kind == 'w') c->disk.fail_write = 1;
        if (kind == 'f') c->disk.fail_flush = 1;
        CHECK(!c->run()); c->disk.hook = {}; c->failed(); ++faults;
    }
    {
        auto c = std::make_unique<Case>(notes); Mount::Registry::Session foreign;
        checked(c->registry.acquire(c->key, {11, 65536}, block::SessionAccess::readonly, foreign), "foreign acquisition");
        CHECK(!c->run() && !c->disk.reads && !c->disk.writes && !c->disk.flushes);
        checked(foreign.close(), "foreign close");
        auto next = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, c->startup.get());
        CHECK(!next->mount(c->registry, c->key, {11, 65536}) && !c->disk.reads); ++rejected;
    }
    std::printf("PASS mounted startup: checks=%zu mounted=%zu rejects=%zu faults=%zu callbacks=%zu same-Owner=1 namespace-cases-here=0 active-reclaim-writing=0\n",
        checks, mounted, rejected, faults, callbacks);
}
