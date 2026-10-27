#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4/extent.hpp>
#include <ok/fs/ext4/alloc.hpp>
#include <type_traits>
#define private public
#include <ok/fs/ext4/txn.hpp>
#undef private
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace zero_preview_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate Session preview model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Registry = block::BlockSessions<2>;
using Session = Registry::Session;
using Preview = Ext4ZeroTruncateSessionPreview<Session>;
static_assert(!std::is_copy_constructible_v<Preview> && !std::is_move_constructible_v<Preview>);
alignas(Preview) std::array<std::byte, sizeof(Preview)> arena{};
Ext4ZeroTruncatePlan planner{}, plan{};
Ext4ZeroTruncateWitnessScratch witness{};
std::array<Block, 8705> original_disk{};
usize positive{}, refusal{}, read_faults{}, drift_cases{};
constexpr u32 sequence = 71, journal_first = 1000;

void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void stamp(Block &bytes, usize at, u32 seed)
{ be32(bytes, at, 0); be32(bytes, at, jbd2::detail::crc32c(seed, bytes)); }
void journal(Image &image, u32 mask)
{
    const auto request = image.update();
    const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
        image.blocks[image.f.group ? 8195 : 3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, request, source, planner, plan));
    auto &head = image.blocks[journal_first];
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1);
    be32(head, 24, sequence); be32(head, 28, 1); be32(head, 40, jbd2::feature_checksum_v3);
    for (usize index = 0; index < image.f.binding.uuid.size(); ++index) head[48 + index] = image.f.binding.uuid[index];
    be32(head, 64, 1); head[80] = 4; stamp(head, 252, ~u32{0});
    auto &descriptor = image.blocks[journal_first + 1];
    be32(descriptor, 0, jbd2::magic); be32(descriptor, 4, u32(jbd2::RecordType::descriptor)); be32(descriptor, 8, sequence);
    for (usize home = 0; home < 4; ++home)
    {
        auto &payload = image.blocks[journal_first + 2 + home]; payload = plan.after[home];
        const bool escaped = jbd2::detail::be32(payload, 0) == jbd2::magic;
        if (escaped) be32(payload, 0, 0);
        const usize at = home ? 44 + (home - 1) * 16 : 12;
        const u32 flags = (home ? jbd2::flag_same_uuid : 0) | (home == 3 ? jbd2::flag_last_tag : 0) |
            (escaped ? jbd2::flag_escape : 0);
        be32(descriptor, at, plan.targets[home]); be32(descriptor, at + 4, flags);
        be32(descriptor, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(image.f.seed,
            std::span<const u8>{descriptor}.subspan(8, 4)), payload));
    }
    CHECK(Ext4ZeroTruncateWitnessCodec::encode(image.f.binding, plan, image.blocks[4], sequence, witness, descriptor));
    stamp(descriptor, 1020, image.f.seed);
    auto &commit = image.blocks[journal_first + 6];
    be32(commit, 0, jbd2::magic); be32(commit, 4, u32(jbd2::RecordType::commit)); be32(commit, 8, sequence);
    be32(commit, 52, request.seconds); be32(commit, 56, request.nanoseconds); stamp(commit, 16, image.f.seed);
    image.f.binding.empty_log = false; image.f.binding.journal_sequence = sequence;
    for (usize home = 0; home < 4; ++home)
        image.blocks[plan.targets[home]] = mask & (1U << home) ? plan.after[home] : plan.before[home];
}

struct Device final : driver::BlockDevice
{
    Image &image;
    u64 origin{}, sectors{};
    usize reads{}, writes{}, flushes{}, fail_at{}, hook_read{};
    mutable usize geometry_calls{}, hook_geometry{};
    void (*hook)(void *){};
    void *context{};
    Device(Image &source, u64 volume_origin) : image(source), origin(volume_origin), sectors(origin + image.blocks.size() * 2 + 8) {}
    driver::BlockGeometry geometry() const override
    {
        if (++geometry_calls == hook_geometry && hook) hook(context);
        return {sectors, 512, true};
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == hook_read && hook) hook(context);
        if (reads == fail_at) return Status::io_error("injected zero truncate Session READ failure");
        if (first < origin || (first - origin) % 2 || out.size() != 1024 || (first - origin) / 2 >= image.blocks.size())
            return Status::invalid_argument("zero truncate Session model raw bounds");
        const auto &source = image.blocks[(first - origin) / 2];
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(source[at]);
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return {}; }
    Status flush() override { ++flushes; return {}; }
};

usize run(u32 count = 3, u32 mask = 0, bool cross = false, usize fault = 0, u64 volume_first = 0)
{
    Image image(count, cross); journal(image, mask); image.f.binding.layout.first_sector = volume_first;
    original_disk = image.blocks;
    Device device(image, 11 + volume_first);
    Registry registry; Session session;
    const auto key = registry.register_device(device, 7); CHECK(key);
    CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, block::SessionAccess::readonly, session));
    device.fail_at = fault;
    auto *value = ::new (arena.data()) Preview{};
    const auto status = value->prepare(session, image.f.binding);
    const auto result = value->report(); const auto actual = session.report(); CHECK(actual);
    if (!fault)
    {
        CHECK(status); CHECK(result.phase == Ext4ZeroTruncateSessionPreviewPhase::ready);
        CHECK(result.preview.phase == Ext4ZeroTruncatePreviewPhase::ready && result.preview.after_mask == mask);
        CHECK(result.attempts == device.reads && result.completions == device.reads && !result.dispatch_pending);
        CHECK(result.preview.read_attempts == device.reads && result.preview.completed_reads == device.reads);
        CHECK(result.reserved.reads == 2866 + 1062 * image.f.binding.metadata_count / 4);
        CHECK(result.reserved.reads >= device.reads && actual.value().state == block::DeviceState::owned);
        CHECK(session.close()); ++positive;
    }
    else
    {
        CHECK(!status && status.code() == StatusCode::io_error && result.phase == Ext4ZeroTruncateSessionPreviewPhase::failed);
        CHECK(result.attempts == fault && result.completions == fault - 1 && !result.dispatch_pending);
        CHECK(result.preview.read_attempts == fault && result.preview.completed_reads == fault - 1);
        CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().reads == fault &&
            actual.value().completed_reads == fault - 1 && actual.value().first_error == StatusCode::io_error);
        CHECK(!session.close()); ++refusal; ++read_faults;
    }
    const auto prior = device.reads;
    CHECK(!value->prepare(session, image.f.binding)); CHECK(device.reads == prior);
    CHECK(device.writes == 0 && device.flushes == 0 && image.blocks == original_disk);
    value->~Preview(); return device.reads;
}

void capacities()
{
    block::SessionReport initial{};
    initial.state = block::DeviceState::owned; initial.identity = 7; initial.registration_generation = 1; initial.session_serial = 1;
    for (u32 groups = 1; groups <= 32; ++groups)
    {
        const usize cap = 2866 + 1062 * groups;
        const auto valid = Preview::capacity<u16>(initial, groups);
        CHECK(valid && valid.value().reads == cap && valid.value().groups == groups); ++positive;
        auto boundary = initial; boundary.reads = boundary.completed_reads = 0xffffU - cap;
        CHECK(Preview::capacity<u16>(boundary, groups)); ++positive;
        ++boundary.reads; ++boundary.completed_reads;
        CHECK(!Preview::capacity<u16>(boundary, groups)); ++refusal;
    }
    CHECK(Preview::capacity(initial, 32).value().reads == 36850);
    CHECK(!Preview::capacity(initial, 0)); CHECK(!Preview::capacity(initial, 33)); refusal += 2;
    CHECK(!Preview::capacity<u8>(initial, 1)); ++refusal;
    auto boundary = initial; boundary.reads = boundary.completed_reads = ~usize{0} - 36850;
    CHECK(Preview::capacity(boundary, 32)); ++positive;
    ++boundary.reads; ++boundary.completed_reads; CHECK(!Preview::capacity(boundary, 32)); ++refusal;
    for (usize change = 0; change < 9; ++change)
    {
        auto altered = initial;
        if (change == 0) altered.state = block::DeviceState::ready;
        if (change == 1) altered.identity = 0;
        if (change == 2) altered.registration_generation = 0;
        if (change == 3) altered.session_serial = 0;
        if (change == 4) altered.first_error = StatusCode::io_error;
        if (change == 5) altered.in_flight = true;
        if (change == 6) altered.pending_writes = true;
        if (change == 7) ++altered.reads;
        if (change == 8) ++altered.writes;
        CHECK(!Preview::capacity(altered, 1)); ++refusal;
    }
}

struct Mutation
{
    Preview &value;
    Session &foreign;
    usize kind{};
    static void apply(void *raw)
    {
        auto &m = *static_cast<Mutation *>(raw); auto &v = m.value;
        switch (m.kind)
        {
        case 0: v.original_ = &m.foreign; break;
        case 1: ++v.initial_.session_serial; break;
        case 2: ++v.initial_seal_.reads; break;
        case 3: ++v.progress_.reads; break;
        case 4: ++v.progress_seal_.completed_reads; break;
        case 5: ++v.geometry_.block_count; break;
        case 6: ++v.geometry_seal_.block_size; break;
        case 7: ++v.bounds_.first; break;
        case 8: ++v.bounds_seal_.count; break;
        case 9: v.binding_.uuid[0] ^= 1; break;
        case 10: ++v.binding_seal_.metadata.back().count; break;
        case 11: ++v.control_.attempts; break;
        case 12: ++v.sealed_.completions; break;
        case 13: ++v.control_.groups; break;
        case 14: ++v.reserved_.reads; break;
        case 15: ++v.reserved_.groups; break;
        case 16: ++v.control_.sector; break;
        case 17: v.sealed_.pending = false; break;
        case 18: ++v.flight_.reads; break;
        case 19: ++v.flight_seal_.completed_reads; break;
        case 20: v.control_.phase = Ext4ZeroTruncateSessionPreviewPhase::ready; break;
        case 21: v.original_seal_ = &m.foreign; break;
        default: std::abort();
        }
    }
};

void retained_drift()
{
    for (usize stage = 0; stage < 2; ++stage)
        for (usize kind = 0; kind < 22; ++kind)
        {
            Image image(3); journal(image, 0); original_disk = image.blocks;
            Device device(image, 11), other_device(image, 11);
            Registry registry; Session session, foreign;
            const auto key = registry.register_device(device, 7), other = registry.register_device(other_device, 8);
            CHECK(key && other);
            CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, block::SessionAccess::readonly, session));
            CHECK(registry.acquire(other.value(), {11, other_device.sectors - 11}, block::SessionAccess::readonly, foreign));
            auto *value = ::new (arena.data()) Preview{};
            Mutation mutation{*value, foreign, kind}; device.context = &mutation; device.hook = Mutation::apply;
            if (!stage) device.hook_geometry = 2; else device.hook_read = 1;
            CHECK(!value->prepare(session, image.f.binding));
            const auto actual = session.report(), untouched = foreign.report(); CHECK(actual && untouched);
            CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().reads == 1 &&
                actual.value().completed_reads == 0 && !actual.value().in_flight);
            const auto report = value->report();
            CHECK(report.observed.state == block::DeviceState::quarantined && report.observed.reads == 1 &&
                report.observed.completed_reads == 0 && !report.observed.in_flight && report.observed.identity == 7);
            CHECK(device.reads == stage && !device.writes && !device.flushes && image.blocks == original_disk);
            CHECK(untouched.value().state == block::DeviceState::owned && untouched.value().reads == 0 && !other_device.reads);
            const auto prior = device.reads; CHECK(!value->prepare(session, image.f.binding)); CHECK(device.reads == prior);
            value->~Preview(); CHECK(foreign.close()); ++refusal; ++drift_cases;
        }
}

void admission_refusals()
{
    for (usize kind = 0; kind < 8; ++kind)
    {
        Image image(3); journal(image, 0); Device device(image, 11); Registry registry; Session session;
        const auto key = registry.register_device(device, 7); CHECK(key);
        CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, block::SessionAccess::readonly, session));
        auto binding = image.f.binding;
        if (kind == 0) binding.metadata_count = 0;
        if (kind == 1) --binding.metadata_count;
        if (kind == 2) binding.metadata_count = binding.metadata.size() + 4;
        if (kind == 3) binding.layout.first_sector = device.sectors;
        if (kind == 4) binding.layout.filesystem_blocks = device.sectors;
        if (kind == 5) binding.layout.block_size = 4096;
        if (kind == 6) binding.layout.filesystem_blocks = 0;
        if (kind == 7) CHECK(session.close());
        auto *value = ::new (arena.data()) Preview{};
        CHECK(!value->prepare(session, binding)); CHECK(!device.reads && !device.writes && !device.flushes);
        CHECK(!value->prepare(session, image.f.binding)); CHECK(!device.reads);
        value->~Preview(); ++refusal;
    }
    // A copied public report has no retained anchor, workspace claim or READ route.
    Image image(3); journal(image, 0); Device device(image, 11); Registry registry; Session session;
    const auto key = registry.register_device(device, 7); CHECK(key);
    CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, block::SessionAccess::readonly, session));
    auto *value = ::new (arena.data()) Preview{};
    CHECK(value->prepare(session, image.f.binding));
    auto copy = value->report(); copy.reserved.reads = ~usize{0}; copy.observed.session_serial = 99;
    CHECK(value->report().reserved.reads != copy.reserved.reads && value->report().observed.session_serial != copy.observed.session_serial);
    CHECK(device.reads == 54 && !device.writes && !device.flushes); CHECK(session.close());
    value->~Preview(); ++positive;
}
} // namespace

int main()
{
    capacities();
    for (u32 count : {1U, 3U, 16U})
        for (u32 mask = 0; mask < 16; ++mask) run(count, mask);
    for (u32 mask = 0; mask < 16; ++mask) run(3, mask, true);
    CHECK(run(3, 0, false, 0, 4) == 54);
    const auto baseline = run(); CHECK(baseline == 54);
    for (usize fault = 1; fault <= baseline; ++fault) run(3, 0, false, fault);
    retained_drift(); admission_refusals();
    std::printf("PASS hosted zero-truncate guarded Session preview positive=%zu refusal=%zu read_faults=%zu drift_cases=%zu max_reads=36850 baseline_reads=%zu\n",
        positive, refusal, read_faults, drift_cases, baseline);
    return 0;
}
