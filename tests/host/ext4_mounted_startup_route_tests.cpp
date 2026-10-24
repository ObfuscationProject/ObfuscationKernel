// Compile this private integration test with -fno-access-control.
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "startup route:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

using Mount = Ext4MountedMetadata<2>;
Bytes load(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input);
    Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() == 32 * 1024 * 1024);
    return bytes;
}

struct Case
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Ext4MountedReclaimWorkspace> reclaim = std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount = std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime, reclaim.get(), startup.get());

    explicit Case(const Bytes &image) : disk(image)
    {
        disk.origin = 11;
        auto registration = registry.register_device(disk, 49101, 65536);
        CHECK(registration);
        key = registration.value();
    }
    Status run() { return mount->mount(registry, key, {11, 65536}); }
    Ext4ReclaimRouteBudgetReport budget() const { return mount->owner_.startup_route_budget_.report(); }
};

void ordinary(const Bytes &image)
{
    Case c(image);
    usize guarded_misses{};
    c.disk.hook = [&](char operation) {
        if (operation == 'r' && c.mount->owner_.startup_active_ &&
            !c.mount->owner_.startup_route_opening_ && c.mount->owner_.cache_.report().in_flight)
            ++guarded_misses;
    };
    CHECK(c.run());
    const auto budget = c.budget();
    CHECK(budget.phase == Ext4ReclaimRoutePhase::closed && budget.attempted && !budget.refined);
    CHECK(budget.attempts == (std::array<usize, 3>{4, 0, 0}) && budget.completions == budget.attempts);
    CHECK(budget.observed.reads == 4 && budget.observed.completed_reads == 4);
    auto vfs = std::make_unique<VirtualFileSystem>();
    auto publication = publish_owned_root(*c.mount, *vfs); CHECK(publication);
    CHECK(c.disk.reads > 4 && guarded_misses &&
          c.mount->report().phase == Ext4MountPhase::readable);
    CHECK(detach_and_drain(*c.mount, *vfs, publication.value()));
    CHECK(c.mount->freeze_clean());
}

void active_stays_readonly(const Bytes &image)
{
    Case c(image);
    const auto status = c.run();
    const auto budget = c.budget();
    CHECK(status.code() == StatusCode::unsupported);
    CHECK(budget.phase == Ext4ReclaimRoutePhase::closed && budget.attempted && budget.refined);
    CHECK(budget.attempts == (std::array<usize, 3>{c.disk.reads, 0, 0}) &&
          budget.completions == budget.attempts);
    CHECK(budget.observed.reads == c.disk.reads && budget.observed.completed_reads == c.disk.reads);
    CHECK(c.disk.reads > 5 && !c.disk.writes && !c.disk.flushes && c.disk.live == image);
    CHECK(c.mount->report().phase == Ext4MountPhase::failed && !c.mount->root());
}

void callback_fault(const Bytes &image, bool detach)
{
    Case c(image);
    bool fired{};
    c.disk.hook = [&](char operation) {
        CHECK(operation == 'r');
        if (fired) return;
        fired = true;
        if (detach) c.mount->owner_.startup_route_opening_ = false;
        else ++c.mount->owner_.startup_route_budget_.control_.attempts[0];
    };
    const auto status = c.run();
    CHECK(!status && fired && c.disk.reads == 1 && !c.disk.writes && !c.disk.flushes);
    CHECK(c.disk.live == image && c.disk.durable == image && !c.mount->root());
    CHECK(c.mount->report().phase == Ext4MountPhase::failed);
    CHECK(c.registry.inspect(c.key).value().state == block::DeviceState::quarantined);
}

void canonical_miss_fault(const Bytes &image)
{
    Case c(image);
    bool fired{};
    c.disk.hook = [&](char operation) {
        if (fired || operation != 'r' || !c.mount->owner_.startup_active_ ||
            c.mount->owner_.startup_route_opening_ || !c.mount->owner_.cache_.report().in_flight)
            return;
        fired = true;
        ++c.mount->owner_.reclaim_io_.reads;
    };
    CHECK(!c.run() && fired);
    CHECK(c.budget().phase == Ext4ReclaimRoutePhase::closed);
    CHECK(c.mount->report().phase == Ext4MountPhase::failed && !c.mount->root());
    CHECK(c.registry.inspect(c.key).value().state == block::DeviceState::quarantined);
}

void transport_failure_priority(const Bytes &image, usize variant)
{
    const bool corrupt_finish = variant & 1, detach = variant & 2;
    Case c(image);
    Disk decoy_disk(image);
    decoy_disk.origin = 11;
    const auto decoy_registration = c.registry.register_device(decoy_disk, 49102, 65536);
    CHECK(decoy_registration);
    Mount::Registry::Session decoy_session;
    CHECK(c.registry.acquire(decoy_registration.value(), {11, 65536},
                             block::SessionAccess::writable, decoy_session));
    const auto decoy_before = decoy_session.report();
    CHECK(decoy_before);
    c.disk.fail_read = 1;
    bool fired{};
    c.disk.hook = [&](char operation) {
        CHECK(operation == 'r' && !fired);
        fired = true;
        CHECK(c.mount->owner_.startup_route_opening_);
        CHECK(c.mount->owner_.startup_route_budget_.in_flight_exact(c.mount->owner_.cache_.session_));
        if (corrupt_finish) ++c.mount->owner_.startup_route_budget_.control_.attempts[0];
        if (detach) c.mount->owner_.startup_route_opening_ = false;
    };
    const auto status = c.run();
    CHECK(status.code() == StatusCode::io_error && fired);
    CHECK(c.mount->owner_.startup_route_opening_ == !detach);
    CHECK(c.disk.reads == 1 && !c.disk.writes && !c.disk.flushes);
    CHECK(c.disk.live == image && c.disk.durable == image && !c.mount->root());
    CHECK(c.mount->report().phase == Ext4MountPhase::failed &&
          c.mount->owner_.startup_report().status.code() == StatusCode::io_error);
    const auto actual = c.mount->owner_.cache_.session_.report();
    CHECK(actual);
    CHECK(actual.value().identity == 49101 && actual.value().session_serial == 1);
    CHECK(c.mount->owner_.cache_.report().first_sector == 11 &&
          c.mount->owner_.cache_.report().sectors == 65536 &&
          c.mount->owner_.cache_.session_.geometry().block_count == 65536);
    CHECK(actual.value().state == block::DeviceState::quarantined &&
          actual.value().first_error == StatusCode::io_error && !actual.value().in_flight);
    CHECK(actual.value().reads == 1 && !actual.value().completed_reads &&
          !actual.value().writes && !actual.value().flushes && !actual.value().pending_writes);
    CHECK(!c.mount->owner_.reclaim_io_.reads && !c.mount->owner_.reclaim_io_.completed_reads);
    CHECK(c.budget().phase == Ext4ReclaimRoutePhase::failed);
    CHECK(c.registry.inspect(c.key).value().state == block::DeviceState::quarantined);
    std::array<std::byte, 1024> blocked;
    blocked.fill(std::byte{0x5a});
    CHECK(!c.mount->owner_.startup_read_blocks(2, blocked));
    CHECK(std::all_of(blocked.begin(), blocked.end(), [](std::byte value) { return value == std::byte{0x5a}; }));
    CHECK(c.disk.reads == 1 && reclaim_history::same_session(actual.value(),
          c.mount->owner_.cache_.session_.report().value()));
    CHECK(reclaim_history::same_session(decoy_before.value(), decoy_session.report().value()));
    CHECK(!decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes);
    CHECK(decoy_session.close());
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::filesystem::path inputs = argv[1];
    const auto ordinary_image = load(inputs / "finished-chain2.img");
    ordinary(ordinary_image);
    for (const char *name : {"reclaim-committed-before.img", "reclaim-committed-prefix.img",
                             "reclaim-committed-after.img"})
        active_stays_readonly(load(inputs / name));
    callback_fault(ordinary_image, false);
    callback_fault(ordinary_image, true);
    canonical_miss_fault(ordinary_image);
    for (usize variant = 0; variant < 4; ++variant)
        transport_failure_priority(ordinary_image, variant);
    std::printf("PASS mounted startup opening route: checks=%zu ordinary=1 active_readonly=3 callback_faults=3 guarded_pin=1 transport_priority=4 original_session_abort=4 decoy_untouched=4\n", checks);
}
