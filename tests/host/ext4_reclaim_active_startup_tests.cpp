// Hosted real private Owner recovery. No target, SDK, guest or physical claim.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_composed_compatibility_main
#include "ext4_reclaim_composed_history_tests.cpp"
#undef main
#pragma GCC diagnostic pop
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>

namespace
{
using Mount = Ext4MountedMetadata<2>;
using Owner = Ext4OwnedTransaction<2, 8>;
static_assert(!std::is_copy_constructible_v<Owner::StartupReclaimSeal> &&
              !std::is_move_constructible_v<Owner::StartupReclaimSeal>);
static_assert(!std::is_copy_constructible_v<Owner::StartupReclaimLedger> &&
              !std::is_move_constructible_v<Owner::StartupReclaimLedger>);
usize active_completed{}, exact_volumes{}, active_writes{}, active_flushes{}, active_failures{}, authority_failures{};
void mounted_case(const ActiveFixture &fixture, unsigned mask, const Image &golden)
{
    owned_fixture::Disk disk(fixture.mixture(mask)); disk.origin = 37;
    Registry registry; const auto registration = registry.register_device(disk, 50138); CHECK(registration);
    auto workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get());
    auto &owner = mount->owner_;
    const auto status = mount->mount(registry, registration.value(), {37, 65536});
    if (!status) std::fprintf(stderr, "active mount members=%zu mask=%u reads=%zu writes=%zu flushes=%zu: %u %.*s\n",
        fixture.members, mask, disk.reads, disk.writes, disk.flushes, unsigned(status.code()),
        int(status.message().size()), status.message().data());
    CHECK(status && mount->root());
    CHECK(disk.live == golden && disk.durable == golden && disk.pending.empty()); ++exact_volumes;
    const auto report = owner.startup_report(); const auto route = owner.startup_route_budget_.report();
    const auto &seal = owner.startup_reclaim_seal_;
    CHECK(report.phase == Ext4StartupPhase::complete && report.strict_ordinary_admission);
    CHECK(report.initial_family == Ext4StartupFamily::reclaim_active && report.reclaimed == fixture.members);
    CHECK(seal.control.phase == Owner::StartupReclaimPhase::complete && seal.control.prefix.completed_rows == seal.rows);
    CHECK(seal.origin_generation == 1 && owner.startup_generation_ == 1 &&
        seal.control.attachment_generation == seal.rows + 1);
    CHECK(route.phase == Ext4ReclaimRoutePhase::closed && !route.dispatch_pending);
    // The original opening domain ends before root publication; its readonly
    // domain guard deliberately refuses reuse after successful admission.
    CHECK(!owner.startup_reclaim_composed_.stable());
    CHECK(owner.startup_reclaim_history_.domain_.epoch == 1 && owner.startup_reclaim_history_.domain_.generation == 1);
    for (usize row = 0; row < seal.rows; ++row)
        CHECK(Owner::StartupReclaimHistory::same_row(workspace->reclaim_composed_.rows_[row],
            workspace->reclaim_composed_.sealed_rows_[row]));
    CHECK(report.observed.writes == disk.writes && report.observed.completed_writes == disk.writes &&
        report.observed.flushes == disk.flushes && report.observed.completed_flushes == disk.flushes);
    CHECK(disk.flushes == 5 + 8 * (fixture.members - 1));
    CHECK(owner.cache_.transaction_serial_ == seal.initial_transaction + seal.rows);
    active_writes += disk.writes; active_flushes += disk.flushes; ++active_completed;
    std::printf("CASE active Owner members=%zu mask=%u reads=%zu writes=%zu flushes=%zu whole_external_golden=1\n",
        fixture.members, mask, disk.reads, disk.writes, disk.flushes); std::fflush(stdout);
}
void failure_case(const ActiveFixture &fixture, char operation, usize ordinal, bool after_effect = false)
{
    owned_fixture::Disk disk(fixture.mixture(0)); disk.origin = 37;
    if (operation == 'w') { if (after_effect) disk.fail_write_after = ordinal; else disk.fail_write = ordinal; }
    if (operation == 'f') { if (after_effect) disk.fail_flush_after = ordinal; else disk.fail_flush = ordinal; }
    Registry registry; const auto registration = registry.register_device(disk, 50139); CHECK(registration);
    auto workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get());
    const auto status = mount->mount(registry, registration.value(), {37, 65536});
    auto &owner = mount->owner_;
    CHECK(!status && !mount->root());
    CHECK(owner.startup_report().phase == Ext4StartupPhase::failed);
    const auto actual = registry.inspect(registration.value()); CHECK(actual);
    CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().first_error == StatusCode::io_error);
    CHECK(!actual.value().in_flight && !disk.trace.empty() && disk.trace.back().operation == operation);
    if (operation == 'w') CHECK(actual.value().writes == ordinal && actual.value().completed_writes + 1 == ordinal);
    if (operation == 'f') CHECK(actual.value().flushes == ordinal && actual.value().completed_flushes + 1 == ordinal);
    const auto count = disk.trace.size();
    CHECK(!owner.startup_reclaim_recover() && !owner.startup_admit());
    CHECK(disk.trace.size() == count && !mount->root());
    ++active_failures;
    std::printf("CASE active transport failure operation=%c ordinal=%zu after_effect=%u no_fallthrough=1\n",
        operation, ordinal, unsigned(after_effect)); std::fflush(stdout);
}
void callback_case(const ActiveFixture &fixture, char operation, usize ledger_field = 0)
{
    owned_fixture::Disk disk(fixture.mixture(0)); disk.origin = 37;
    Registry registry; const auto registration = registry.register_device(disk, 50140); CHECK(registration);
    auto workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get());
    auto &owner = mount->owner_; bool changed{};
    disk.hook = [&](char selected) {
        if (!changed && selected == operation && owner.startup_reclaim_seal_.minted)
        {
            CHECK(owner.cache_.session_.report().value().in_flight);
            CHECK(!mount->root());
            // A successful runtime child must not authorize a forged ordinary
            // report or a replacement original-domain generation.
            if (operation == 'r' && ledger_field == 0) owner.startup_report_.initial_family = Ext4StartupFamily::ordinary;
            if (ledger_field == 1 || ledger_field == 2)
            {
                const usize selected = operation == 'r' ? 0 : operation == 'w' ? 1 : 2;
                owner.startup_reclaim_ledger_.operation_ = Ext4ReclaimRouteOperation((selected + ledger_field) % 3);
            }
            if (operation == 'r' && ledger_field == 3)
            {
                ++owner.startup_reclaim_ledger_.before_.reads; ++owner.startup_reclaim_ledger_.before_.completed_reads;
                ++owner.reclaim_io_.reads; ++owner.reclaim_io_.completed_reads;
            }
            if (ledger_field == 4) ++owner.cache_.session_.first_;
            if (operation == 'w' && ledger_field == 0) ++owner.startup_reclaim_history_.domain_.generation;
            if (operation == 'f' && ledger_field == 0) owner.startup_reclaim_seal_.control.attachment_generation = 9;
            changed = true;
        }
    };
    CHECK(!mount->mount(registry, registration.value(), {37, 65536}) && changed && !mount->root());
    const auto actual = registry.inspect(registration.value()); CHECK(actual);
    CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().first_error == StatusCode::fault);
    CHECK(!disk.trace.empty() && disk.trace.back().operation == operation);
    const auto route = owner.startup_route_budget_.report();
    CHECK(route.attempts == (std::array<usize, 3>{actual.value().reads, actual.value().writes, actual.value().flushes}));
    CHECK(route.completions == (std::array<usize, 3>{actual.value().completed_reads,
        actual.value().completed_writes, actual.value().completed_flushes}));
    CHECK(!route.dispatch_pending && route.phase == Ext4ReclaimRoutePhase::failed);
    if (operation == 'r') CHECK(!disk.writes && !disk.flushes);
    if (operation == 'w') CHECK(disk.writes == 1 && actual.value().completed_writes == 0);
    if (operation == 'f') CHECK(disk.flushes == 1 && actual.value().completed_flushes == 0);
    const auto count = disk.trace.size();
    CHECK(!owner.startup_admit() && !owner.startup_reclaim_recover() && disk.trace.size() == count);
    ++authority_failures;
    std::printf("CASE active callback original authority operation=%c ledger_field=%zu exact_pending_prefix=1 success_not_adopted=1\n",
        operation, ledger_field);
    std::fflush(stdout);
}
void geometry_callback_case(const ActiveFixture &fixture, char operation, bool volume_bounds = false)
{
    owned_fixture::Disk disk(fixture.mixture(0)); disk.origin = 37;
    struct Device final : driver::BlockDevice
    {
        explicit Device(owned_fixture::Disk &disk) : backing(disk) {}
        owned_fixture::Disk &backing;
        std::function<void()> geometry_callback{};
        driver::BlockGeometry geometry() const override
        { if (geometry_callback) geometry_callback(); return backing.geometry(); }
        Status read_blocks(u64 first, std::span<std::byte> bytes) override
        { return backing.read_blocks(first, bytes); }
        Status write_blocks(u64 first, std::span<const std::byte> bytes) override
        { return backing.write_blocks(first, bytes); }
        Status flush() override { return backing.flush(); }
    } device(disk);
    Registry registry; const auto registration = registry.register_device(device, 50141); CHECK(registration);
    auto workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get());
    auto &owner = mount->owner_; bool changed{}; usize selected_trace{}, selected_count{};
    const auto selected_operation = operation == 'r' ? Ext4ReclaimRouteOperation::read :
        operation == 'w' ? Ext4ReclaimRouteOperation::write : Ext4ReclaimRouteOperation::flush;
    device.geometry_callback = [&]() {
        if (!changed && owner.startup_reclaim_seal_.minted &&
            owner.startup_route_budget_.control_.operation == selected_operation)
        {
            CHECK(owner.cache_.session_.report().value().in_flight);
            selected_trace = disk.trace.size();
            selected_count = operation == 'r' ? disk.reads : operation == 'w' ? disk.writes : disk.flushes;
            if (volume_bounds) ++owner.cache_.session_.first_;
            else ++owner.startup_reclaim_history_.domain_.generation;
            changed = true;
        }
    };
    CHECK(!mount->mount(registry, registration.value(), {37, 65536}) && changed && !mount->root());
    CHECK(disk.trace.size() == selected_trace);
    CHECK(selected_count == (operation == 'r' ? disk.reads : operation == 'w' ? disk.writes : disk.flushes));
    const auto actual = registry.inspect(registration.value()); CHECK(actual);
    CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().first_error == StatusCode::fault);
    if (operation == 'r') CHECK(actual.value().reads == selected_count + 1 && actual.value().completed_reads == selected_count);
    if (operation == 'w') CHECK(actual.value().writes == selected_count + 1 && actual.value().completed_writes == selected_count);
    if (operation == 'f') CHECK(actual.value().flushes == selected_count + 1 && actual.value().completed_flushes == selected_count);
    const auto route = owner.startup_route_budget_.report();
    CHECK(route.attempts == (std::array<usize, 3>{actual.value().reads, actual.value().writes, actual.value().flushes}));
    CHECK(route.completions == (std::array<usize, 3>{actual.value().completed_reads,
        actual.value().completed_writes, actual.value().completed_flushes}));
    CHECK(!route.dispatch_pending && route.phase == Ext4ReclaimRoutePhase::failed);
    const auto count = disk.trace.size();
    CHECK(!owner.startup_admit() && !owner.startup_reclaim_recover() && disk.trace.size() == count);
    ++authority_failures;
    std::printf("CASE active geometry callback original authority selected_operation=%c live_volume_bounds=%u transport_not_dispatched=1 completion_credit=0 no_next_write=1\n",
        operation, unsigned(volume_bounds));
    std::fflush(stdout);
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); const std::string root = argv[1];
    const auto image = [&](const char *name) { return read_image((root + "/" + name).c_str()); };
    ActiveFixture zero(image("input-empty.img")), one(image("input-chain2-cross-group.img")), seven(image("input-chain8-cross-group.img"));
    const auto empty_golden = image("finished-empty.img");
    for (unsigned mask = 0; mask < (1U << zero.plan.reclaim.count); ++mask)
        if (legal(zero.plan.reclaim.count, mask)) mounted_case(zero, mask, empty_golden);
    mounted_case(one, 0, image("finished-chain2.img"));
    mounted_case(seven, (1U << seven.plan.reclaim.count) - 1, image("finished-chain8.img"));
    // Every dispatched WRITE and all five required active FLUSHes are failed
    // in fresh original Owners. Error-after-effect is a separate fault case.
    for (usize ordinal = 1; ordinal <= zero.plan.reclaim.count + 1; ++ordinal) failure_case(zero, 'w', ordinal);
    for (usize ordinal = 1; ordinal <= 5; ++ordinal) failure_case(zero, 'f', ordinal);
    failure_case(zero, 'w', 1, true); failure_case(zero, 'f', 5, true);
    for (char operation : {'r', 'w', 'f'}) callback_case(zero, operation);
    for (char operation : {'r', 'w', 'f'})
        for (usize substitute = 1; substitute <= 2; ++substitute) callback_case(zero, operation, substitute);
    callback_case(zero, 'r', 3);
    for (char operation : {'r', 'w', 'f'}) geometry_callback_case(zero, operation);
    callback_case(zero, 'f', 4);
    geometry_callback_case(zero, 'f', true);
    std::printf("PASS active private Owner hosted controls: completed=%zu whole_external_goldens=%zu writes=%zu flushes=%zu active_transport_failures=%zu callback_authority_failures=%zu copied_attachment=0 origin_rebased=0 target=0 guest=0 physical=0\n",
        active_completed, exact_volumes, active_writes, active_flushes, active_failures, authority_failures);
}
