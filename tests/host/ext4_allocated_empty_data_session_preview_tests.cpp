// Reuse the exact accepted pure-media fixture, keeping its useful regressions intact.
#define main allocated_empty_pure_regressions
#include "ext4_allocated_empty_data_preview_tests.cpp"
#undef main
#define private public
#include <ok/fs/ext4/alloc.hpp>
#include <ok/fs/ext4/txn.hpp>
#undef private
#include <new>

namespace session_model
{
using Registry = block::BlockSessions<2>;
using Session = Registry::Session;
using Guarded = Ext4AllocatedEmptyDataSessionPreview<Session>;
using Phase = Ext4AllocatedEmptyDataSessionPreviewPhase;
static_assert(!std::is_copy_constructible_v<Guarded> && !std::is_move_constructible_v<Guarded>);
alignas(Guarded) std::array<std::byte, sizeof(Guarded)> arena{};
usize successes{}, faults{}, changes{}, capacities{}, alias_checks{}, grammar{};

struct Device final : driver::BlockDevice
{
    std::span<Block> blocks;
    u64 origin{}, offset{};
    usize reads{}, writes{}, flushes{}, fail_at{}, drift_at{}, hook_read{};
    mutable usize geometry_calls{}, hook_geometry{};
    bool partial{}, unavailable{};
    void (*hook)(void *){};
    void *context{};
    explicit Device(std::span<Block> storage, u64 partition, u64 first)
        : blocks(storage), origin(partition), offset(first) {}
    driver::BlockGeometry geometry() const override
    {
        if (++geometry_calls == hook_geometry && hook) hook(context);
        return {origin + offset + blocks.size() * 2 + 8, 512, true};
    }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == hook_read && hook) hook(context);
        if (sector < origin + offset || (sector - origin - offset) % 2 || out.size() != 1024 ||
            (sector - origin - offset) / 2 >= blocks.size())
            return Status::invalid_argument("allocated empty Session model raw bounds");
        const auto &source = blocks[(sector - origin - offset) / 2];
        const usize copied = reads == fail_at ? (partial ? 511 : 0) : 1024;
        for (usize at = 0; at < copied; ++at) out[at] = std::byte(source[at]);
        if (reads == fail_at)
            return unavailable ? Status::not_initialized("allocated empty model unavailable") :
                Status::io_error("allocated empty model failed READ");
        if (reads == drift_at) out[900] ^= std::byte{1};
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override { ++writes; return {}; }
    Status flush() override { ++flushes; return {}; }
};

usize run(bool cross, u8 mask, u32 size, u64 origin, u64 offset,
          usize fail = 0, bool partial = false, bool unavailable = false, usize drift = 0)
{
    Image image(0); Fixture second(true);
    if (cross)
    {
        prepare_cross(image, second, false);
        for (usize home = 0; home < 4; ++home)
            cross_disk[plan.targets[home]] = mask & (1U << home) ? plan.after[home] : plan.before[home];
        cross_preserved = cross_disk;
    }
    else prepare(image, mask, size);
    auto &binding = cross ? second.binding : image.f.binding;
    binding.layout.first_sector = offset;
    std::span<Block> media = cross ? std::span<Block>{cross_disk} : std::span<Block>{image.blocks};
    Device device(media, origin, offset); Registry registry; Session original;
    const auto key = registry.register_device(device, 7); CHECK(key);
    const auto geometry = device.geometry();
    CHECK(registry.acquire(key.value(), {origin, geometry.block_count - origin}, block::SessionAccess::readonly, original));
    device.fail_at = fail; device.partial = partial; device.unavailable = unavailable; device.drift_at = drift;
    auto *value = ::new (arena.data()) Guarded{};
    const auto status = value->prepare(original, binding);
    const auto report = value->report(); const auto actual = original.report(); CHECK(actual);
    CHECK(reclaim_history::same_session(report.observed, actual.value()));
    CHECK(report.attempts == device.reads && report.preview.read_attempts == device.reads &&
        report.completions == device.reads - (fail ? 1 : 0) && !report.dispatch_pending);
    CHECK(!actual.value().in_flight && !actual.value().writes && !actual.value().flushes);
    if (!fail && !drift)
    {
        CHECK(status && report.phase == Phase::ready && report.preview.after_mask == mask &&
            report.preview.old_ownership && report.preview.final_ownership && report.preview.snapshot_readback);
        CHECK(report.preview.physical == (cross ? 8261U : 70U) && report.preview.group == (cross ? 1U : 0U));
        CHECK(report.reserved.reads == 2876 + 1062 * binding.metadata_count / 4);
        CHECK(device.reads <= report.reserved.reads && original.close()); ++successes;
    }
    else
    {
        CHECK(!status && report.phase == Phase::failed && actual.value().state == block::DeviceState::quarantined);
        CHECK(actual.value().reads == device.reads && actual.value().completed_reads == report.completions);
        if (fail) CHECK(status.code() == (unavailable ? StatusCode::not_initialized : StatusCode::io_error));
        CHECK(!original.close()); if (fail) ++faults; else ++changes;
    }
    const auto stopped = device.reads;
    CHECK(!value->prepare(original, binding) && device.reads == stopped);
    CHECK(!device.writes && !device.flushes);
    if (cross) CHECK(cross_disk == cross_preserved); else CHECK(image.blocks == preserved);
    value->~Guarded(); return device.reads;
}

void budget_controls()
{
    block::SessionReport initial{};
    initial.state = block::DeviceState::owned; initial.identity = initial.registration_generation = initial.session_serial = 1;
    for (u32 groups = 1; groups <= 32; ++groups)
    {
        const usize count = 2876 + 1062 * groups;
        CHECK(Guarded::capacity(initial, groups).value().reads == count); ++capacities;
        for (bool completed : {false, true})
        {
            auto bound = initial; bound.reads = bound.completed_reads = 0xffffU - count;
            CHECK(Guarded::capacity<u16>(bound, groups)); ++capacities;
            ++bound.reads; ++bound.completed_reads;
            CHECK(!Guarded::capacity<u16>(bound, groups)); ++capacities;
            bound = initial;
            if (completed) bound.completed_reads = ~usize{0}; else bound.reads = ~usize{0};
            CHECK(!Guarded::capacity(bound, groups)); ++capacities;
        }
    }
    CHECK(!Guarded::capacity(initial, 0) && !Guarded::capacity(initial, 33)); capacities += 2;
    CHECK(!Guarded::capacity<u8>(initial, 1)); ++capacities;
    for (u32 field = 0; field < 10; ++field)
    {
        auto invalid = initial;
        if (field == 0) invalid.state = block::DeviceState::ready;
        if (field == 1) invalid.identity = 0;
        if (field == 2) invalid.registration_generation = 0;
        if (field == 3) invalid.session_serial = 0;
        if (field == 4) invalid.first_error = StatusCode::io_error;
        if (field == 5) invalid.in_flight = true;
        if (field == 6) invalid.pending_writes = true;
        if (field == 7) invalid.completed_reads = 1;
        if (field == 8) invalid.completed_writes = 1;
        if (field == 9) invalid.completed_flushes = 1;
        CHECK(!Guarded::capacity(invalid, 1)); ++capacities;
    }
}

struct Mutation { Guarded &owner; Session &foreign; u32 field; };
void mutate(void *context)
{
    auto &value = *static_cast<Mutation *>(context); auto &owner = value.owner;
    switch (value.field)
    {
    case 0: owner.original_ = &value.foreign; break;
    case 1: owner.original_seal_ = &value.foreign; break;
    case 2: owner.anchor_.second_ = &value.foreign; break;
    case 3: ++owner.control_.attempts; break;
    case 4: ++owner.sealed_.completions; break;
    case 5: ++owner.reserved_.reads; break;
    case 6: ++owner.binding_.metadata[127].count; break;
    case 7: ++owner.binding_seal_.journal_sequence; break;
    case 8: ++owner.progress_.reads; break;
    case 9: ++owner.progress_seal_.completed_reads; break;
    case 10: ++owner.initial_.session_serial; break;
    case 11: ++owner.initial_seal_.registration_generation; break;
    case 12: ++owner.geometry_.block_count; break;
    case 13: ++owner.geometry_seal_.block_count; break;
    case 14: ++owner.bounds_.first; break;
    case 15: ++owner.bounds_seal_.count; break;
    case 16: ++owner.flight_.reads; break;
    case 17: ++owner.flight_seal_.completed_reads; break;
    default: CHECK(false);
    }
}

void retained_controls()
{
    for (u32 field = 0; field < 18; ++field) for (bool geometry : {false, true})
    {
        Image image(0); prepare(image, 7); Device device(image.blocks, 19, 3), decoy(image.blocks, 19, 3);
        image.f.binding.layout.first_sector = 3;
        Registry registry; Session original, foreign;
        const auto key = registry.register_device(device, 7), other = registry.register_device(decoy, 9); CHECK(key && other);
        const auto bounds = block::SessionBounds{19, device.geometry().block_count - 19};
        CHECK(registry.acquire(key.value(), bounds, block::SessionAccess::readonly, original));
        CHECK(registry.acquire(other.value(), bounds, block::SessionAccess::readonly, foreign));
        const auto foreign_before = foreign.report(); CHECK(foreign_before);
        auto *value = ::new (arena.data()) Guarded{}; Mutation request{*value, foreign, field};
        device.hook = mutate; device.context = &request;
        if (geometry) device.hook_geometry = device.geometry_calls + 2; else device.hook_read = 1;
        CHECK(!value->prepare(original, image.f.binding));
        const auto report = value->report(); const auto actual = original.report(); const auto untouched = foreign.report();
        CHECK(actual && untouched && report.phase == Phase::failed && !actual.value().in_flight &&
            actual.value().state == block::DeviceState::quarantined && reclaim_history::same_session(report.observed, actual.value()));
        CHECK(reclaim_history::same_session(untouched.value(), foreign_before.value()) && !decoy.reads && !decoy.writes && !decoy.flushes);
        CHECK(!device.writes && !device.flushes && image.blocks == preserved);
        CHECK(foreign.close()); value->~Guarded(); ++changes;
    }
}

void alias_and_grammar_controls()
{
    Image image(0); prepare(image, 0); Device device(image.blocks, 11, 0); Registry registry; Session original;
    const auto key = registry.register_device(device, 7); CHECK(key);
    CHECK(registry.acquire(key.value(), {11, device.geometry().block_count - 11}, block::SessionAccess::readonly, original));
    auto *value = ::new (arena.data()) Guarded{};
    const auto pristine = arena;
    const auto &alias = *reinterpret_cast<const Ext4RecoveryInfo *>(value);
    CHECK(!value->prepare(original, alias) && arena == pristine && !device.reads); ++alias_checks;
    auto &same_session = *reinterpret_cast<Session *>(value);
    CHECK(!value->prepare(same_session, image.f.binding) && arena == pristine && !device.reads); ++alias_checks;
    image.blocks[1007][7] = 1;
    CHECK(!value->prepare(original, image.f.binding) && device.reads == 8 && !device.writes && !device.flushes); ++grammar;
    CHECK(value->report().phase == Phase::failed && !original.report().value().in_flight);
    value->~Guarded();
}
}

int main()
{
    using namespace session_model;
    for (u64 origin : {0ULL, 11ULL}) for (u64 offset : {0ULL, 7ULL})
        for (u32 size : {1U, 73U, 1024U}) for (u8 mask = 0; mask < 16; ++mask)
            CHECK(run(false, mask, size, origin, offset) == 58);
    for (u8 mask = 0; mask < 16; ++mask) CHECK(run(true, mask, 73, 19, 3) == 70);
    for (bool cross : {false, true})
    {
        const usize total = cross ? 70 : 58;
        for (usize at = 1; at <= total; ++at) for (bool partial : {false, true}) for (bool unavailable : {false, true})
            CHECK(run(cross, 5, 73, 19, 3, at, partial, unavailable) == at);
        for (usize at = total - 13; at <= total; ++at) CHECK(run(cross, 5, 73, 19, 3, 0, false, false, at) == at);
    }
    budget_controls(); retained_controls(); alias_and_grammar_controls();
    std::printf("ALLOCATED_EMPTY_SESSION_MODEL positive=%zu faults=%zu changes=%zu budgets=%zu aliases=%zu grammar=%zu no_write_flush=yes\n",
        successes, faults, changes, capacities, alias_checks, grammar);
    return 0;
}
