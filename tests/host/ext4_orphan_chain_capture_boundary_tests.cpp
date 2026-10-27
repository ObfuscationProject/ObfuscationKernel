#include "ok/fs/ext4/recovery.hpp"
#include "borrowed_fixture.hpp"
#include <limits>

// Counter exhaustion below deliberately sets private counters with the host
// compiler's -fno-access-control. It is a labelled model boundary, not a claim
// that this fixture executed SIZE_MAX transport requests. Every Session and
// source object remains alive throughout all capture and checking operations.
namespace
{
using Admission = Ext4OrphanSnapshotAdmission<2>;
usize wrapper_acquire_rejects{}, closed_rejects{}, reacquired_preserved{};
usize exhaustion_rejects{}, destructor_preserved{}, wrapper_lifecycle{}, stale_requests{};

void no_capture_authority(const Harness &owner, usize writes = 0, usize flushes = 0)
{
    CHECK(owner.disk.writes == writes && owner.disk.flushes == flushes);
    for (usize at = 0; at < origin * 512; ++at) CHECK(owner.disk.bytes[at] == 0xa5);
}

void wrapper_acquisition(const Image &image)
{
    {
        Harness owner(image);
        Admission wrapper;
        const auto before = owner.registry.inspect(owner.key).value();
        CHECK(!wrapper.open(owner.registry, owner.key, {origin, 65536}, *owner.storage, *owner.scratch));
        CHECK(!wrapper.report().valid && wrapper.report().phase == Ext4OrphanSnapshotPhase::rejected);
        CHECK(wrapper.report().status.code() == StatusCode::busy);
        const auto after = owner.registry.inspect(owner.key).value();
        CHECK(after.state == block::DeviceState::owned && after.first_error == StatusCode::ok &&
              after.session_serial == before.session_serial && after.reads == before.reads);
        CHECK(!wrapper.close() && !wrapper.open(owner.registry, owner.key, {origin, 65536},
              *owner.storage, *owner.scratch));
        CHECK(owner.disk.reads == 0 && owner.session.close());
        no_capture_authority(owner); ++wrapper_acquire_rejects;
    }
    {
        Disk disk(image); Registry registry;
        const auto old = registry.register_device(disk, 919); CHECK(old);
        CHECK(registry.detach(old.value()));
        const auto fresh = registry.register_device(disk, 920); CHECK(fresh);
        Registry::Session actual_owner;
        CHECK(registry.acquire(fresh.value(), {origin, 65536}, block::SessionAccess::writable, actual_owner));
        auto storage = std::make_unique<Ext4OrphanChainCaptureStorage>();
        auto scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
        Admission wrapper;
        CHECK(!wrapper.open(registry, old.value(), {origin, 65536}, *storage, *scratch));
        CHECK(!wrapper.report().valid && wrapper.report().phase == Ext4OrphanSnapshotPhase::rejected);
        CHECK(wrapper.report().status.code() == StatusCode::not_found);
        const auto report = registry.inspect(fresh.value()); CHECK(report);
        CHECK(report.value().state == block::DeviceState::owned && report.value().first_error == StatusCode::ok &&
              report.value().registration_generation == 2 && report.value().identity == 920);
        CHECK(!wrapper.open(registry, fresh.value(), {origin, 65536}, *storage, *scratch));
        CHECK(!wrapper.close() && disk.reads == 0 && disk.writes == 0 && disk.flushes == 0);
        CHECK(actual_owner.close()); ++wrapper_acquire_rejects;
    }
    {
        Disk disk(image); Registry registry;
        const auto key = registry.register_device(disk, 919); CHECK(key);
        auto storage = std::make_unique<Ext4OrphanChainCaptureStorage>();
        auto scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
        Admission wrapper;
        CHECK(!wrapper.open(registry, key.value(), {origin, 65537}, *storage, *scratch));
        CHECK(!wrapper.report().valid && wrapper.report().phase == Ext4OrphanSnapshotPhase::rejected);
        CHECK(wrapper.report().status.code() == StatusCode::invalid_argument);
        const auto report = registry.inspect(key.value()); CHECK(report);
        CHECK(report.value().state == block::DeviceState::ready && report.value().first_error == StatusCode::ok &&
              !report.value().session_serial);
        Registry::Session next;
        CHECK(registry.acquire(key.value(), {origin, 65536}, block::SessionAccess::readonly, next));
        CHECK(!wrapper.open(registry, key.value(), {origin, 65536}, *storage, *scratch));
        CHECK(!wrapper.close() && disk.reads == 0 && disk.writes == 0 && disk.flushes == 0);
        CHECK(next.close()); ++wrapper_acquire_rejects;
    }
}

void wrapper_close_and_stale_request(const Image &image)
{
    Disk disk(image); Registry registry;
    const auto key = registry.register_device(disk, 919); CHECK(key);
    auto storage = std::make_unique<Ext4OrphanChainCaptureStorage>();
    auto scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Admission wrapper;
    CHECK(!wrapper.close());
    CHECK(wrapper.open(registry, key.value(), {origin, 65536}, *storage, *scratch));
    const auto report = wrapper.report(); const auto reads = disk.reads;
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
    CHECK(!wrapper.plan_head(report.head + 1, report.generation, plan) && !plan.valid);
    CHECK(wrapper.report().valid && disk.reads == reads); ++stale_requests;
    plan.valid = true;
    CHECK(!wrapper.plan_head(report.head, report.generation + 1, plan) && !plan.valid);
    CHECK(wrapper.report().valid && disk.reads == reads); ++stale_requests;
    CHECK(wrapper.plan_head(report.head, report.generation, plan) && plan.valid);
    CHECK(wrapper.close());
    CHECK(!wrapper.report().valid && wrapper.report().phase == Ext4OrphanSnapshotPhase::closed);
    const auto closed_reads = disk.reads;
    plan.valid = true;
    CHECK(!wrapper.plan_head(report.head, report.generation, plan) && !plan.valid);
    CHECK(!wrapper.open(registry, key.value(), {origin, 65536}, *storage, *scratch) && !wrapper.close());
    CHECK(disk.reads == closed_reads && disk.writes == 0 && disk.flushes == 0);
    CHECK(registry.inspect(key.value()).value().state == block::DeviceState::ready);
    ++wrapper_lifecycle;
}

void closed_and_reacquired(const Image &image)
{
    {
        Harness owner(image); CHECK(owner.run());
        const auto report = owner.capture.report(); const auto reads = owner.disk.reads;
        CHECK(owner.session.close());
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        CHECK(!owner.capture.report().valid && owner.capture.report().phase == Ext4OrphanSnapshotPhase::rejected);
        const auto current = owner.registry.inspect(owner.key).value();
        CHECK(current.state == block::DeviceState::ready && current.first_error == StatusCode::ok &&
              current.session_serial == report.session_serial && owner.disk.reads == reads);
        no_capture_authority(owner); ++closed_rejects;
    }
    {
        Harness owner(image); CHECK(owner.run());
        const auto report = owner.capture.report(); CHECK(owner.session.close());
        Registry::Session replacement;
        CHECK(owner.registry.acquire(owner.key, {origin, 65536}, block::SessionAccess::writable, replacement));
        const auto current = replacement.report(); CHECK(current);
        CHECK(current.value().session_serial == report.session_serial + 1);
        const auto reads = owner.disk.reads;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        CHECK(!owner.capture.report().valid && owner.capture.report().phase == Ext4OrphanSnapshotPhase::rejected);
        const auto preserved = replacement.report(); CHECK(preserved);
        CHECK(preserved.value().state == block::DeviceState::owned && preserved.value().first_error == StatusCode::ok &&
              preserved.value().session_serial == current.value().session_serial && owner.disk.reads == reads);
        Block bytes{};
        CHECK(replacement.read_blocks(2, std::as_writable_bytes(std::span{bytes})));
        CHECK(replacement.close()); no_capture_authority(owner); ++reacquired_preserved;
    }
    {
        Harness owner(image);
        const auto original = owner.session.report(); CHECK(original);
        usize reads{};
        {
            Capture borrowed;
            CHECK(borrowed.capture(owner.session, *owner.storage, *owner.scratch));
            CHECK(borrowed.report().valid); reads = owner.disk.reads;
        }
        const auto preserved = owner.session.report(); CHECK(preserved);
        CHECK(preserved.value().state == block::DeviceState::owned && preserved.value().first_error == StatusCode::ok &&
              preserved.value().session_serial == original.value().session_serial && owner.disk.reads == reads);
        CHECK(owner.session.close()); no_capture_authority(owner); ++destructor_preserved;
    }
}

void counter_exhaustion_model(const Image &image)
{
    constexpr usize maximum = std::numeric_limits<usize>::max();
    for (usize baseline : {maximum, maximum - 1})
    {
        Harness owner(image);
        // These are live Session objects with internally consistent injected
        // model counters. No pointer to a destroyed owner or storage is used.
        owner.registry.entries_[0].report.reads = baseline;
        owner.registry.entries_[0].report.completed_reads = baseline;
        CHECK(!owner.run()); owner.rejected();
        CHECK(owner.capture.report().status.code() == StatusCode::overflow);
        CHECK(owner.disk.reads == maximum - baseline);
        CHECK(owner.capture.report().session_reads == maximum - baseline);
        no_capture_authority(owner); ++exhaustion_rejects;
    }
    {
        Harness owner(image); CHECK(owner.run());
        const auto report = owner.capture.report(); const auto reads = owner.disk.reads;
        owner.capture.report_.session_reads = maximum;
        owner.registry.entries_[0].report.reads = maximum;
        owner.registry.entries_[0].report.completed_reads = maximum;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        owner.rejected(); CHECK(owner.capture.report().status.code() == StatusCode::overflow);
        CHECK(owner.disk.reads == reads);
        no_capture_authority(owner); ++exhaustion_rejects;
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const auto filled = orphan_image(read_image(argv[1]), true);
    const auto empty = orphan_image(read_image(argv[2]), false);
    for (const auto *image : {&filled, &empty})
    {
        wrapper_acquisition(*image); wrapper_close_and_stale_request(*image);
        closed_and_reacquired(*image); counter_exhaustion_model(*image);
    }
    std::printf("PASS shared capture boundaries: wrapper_acquire_rejects=%zu closed_session_rejects=%zu reacquired_owner_preserved=%zu counter_exhaustion_rejects=%zu borrowed_destructor_preserves_owner=%zu wrapper_lifecycle_accepts=%zu stale_request_rejects=%zu live_objects_only=1 partition_origin=37 capture_write_flush=0\n",
        wrapper_acquire_rejects, closed_rejects, reacquired_preserved, exhaustion_rejects,
        destructor_preserved, wrapper_lifecycle, stale_requests);
}
