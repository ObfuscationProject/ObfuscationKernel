#include "ext4_zero_truncate_preview_test_support.hpp"
#include <algorithm>
#include <ok/driver/driver.hpp>
#include <ok/smp/smp.hpp>
// Negative controls reach private session and transaction state; build with
// -fno-access-control.
#include <ok/block/sessions.hpp>
#include <ok/fs/ext4/alloc.hpp>
#include <ok/fs/ext4/extent.hpp>
#include <ok/fs/ext4/txn.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace zero_preview_fixture;
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "zero truncate transaction:%d: %s\n", __LINE__, #v); std::abort(); } } while (false)
namespace
{
using Registry = block::BlockSessions<2>;
using Session = Registry::Session;
using Transaction = Ext4ZeroTruncateTransaction<Session>;
using Route = Ext4ZeroTruncateRoute;
using Phase = Ext4ZeroTruncateTransactionPhase;
static_assert(!std::is_copy_constructible_v<Transaction> && !std::is_move_constructible_v<Transaction>);
alignas(Transaction) std::array<std::byte, sizeof(Transaction)> arena{};
std::array<Block, 8705> before{}, durable{};
Ext4ZeroTruncatePlan scratch_plan{}, expected_plan{};
Ext4ZeroTruncateJournalFormatScratch format_scratch{};
Ext4ZeroTruncateJournalFormat expected_journal{};
usize positives{}, refusals{}, faults{}, changes{}, budgets{};
void be32(Block &bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void head(Image &image, u32 sequence = 71)
{
    auto &h = image.blocks[1000]; h = {};
    be32(h, 0, jbd2::magic); be32(h, 4, u32(jbd2::RecordType::superblock_v2));
    be32(h, 12, 1024); be32(h, 16, 1024); be32(h, 20, 1); be32(h, 24, sequence);
    be32(h, 40, jbd2::feature_checksum_v3); be32(h, 64, 1); h[80] = 4;
    for (usize at = 0; at < 16; ++at) h[48 + at] = image.f.binding.uuid[at];
    be32(h, 252, jbd2::detail::crc32c(~u32{0}, h));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = sequence;
    // Stale unused records must be retained before each own successful write.
    for (u32 record = 1; record < 8; ++record)
        for (usize byte = 0; byte < 1024; ++byte) image.blocks[1000 + record][byte] = u8(record * 29 + byte * 13);
}
void expected(Image &image)
{
    const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
        image.blocks[image.f.group ? 8195 : 3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, image.update(), source, scratch_plan, expected_plan));
    CHECK(Ext4ZeroTruncateJournalFormatter::format(image.f.binding, expected_plan, image.blocks[1000], image.blocks[4],
        format_scratch, expected_journal));
}
void active(Image &image, u8 mask)
{
    expected(image);
    image.blocks[1000] = expected_journal.head; image.blocks[1001] = expected_journal.descriptor;
    for (u32 at = 0; at < 4; ++at) image.blocks[1002 + at] = expected_journal.payloads[at];
    image.blocks[1006] = expected_journal.commit; image.blocks[1007] = expected_journal.end;
    for (u32 at = 0; at < 4; ++at) image.blocks[expected_plan.targets[at]] = mask & (1U << at) ? expected_plan.after[at] : expected_plan.before[at];
    image.f.binding.empty_log = false; image.f.binding.journal_sequence = expected_journal.active_sequence;
}
struct Device final : driver::BlockDevice
{
    Image &image;
    u64 origin{}, volume_offset{7}, sectors{};
    usize reads{}, writes{}, flushes{}, fail_number{}, hook_number{};
    u32 fail_kind{}, hook_kind{};
    void (*hook)(void *){}; void *context{};
    bool recovery{}, barriers{};
    Device(Image &value, u64 partition_origin) : image(value), origin(partition_origin), sectors(origin + volume_offset + image.blocks.size() * 2 + 8) {}
    driver::BlockGeometry geometry() const override { return {sectors, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; if (hook_kind == 1 && reads == hook_number && hook) hook(context);
        if (fail_kind == 1 && reads == fail_number) return Status::io_error("model selected READ failure");
        if (sector < origin + volume_offset || (sector - origin - volume_offset) % 2 || out.size() != 1024 ||
            (sector - origin - volume_offset) / 2 >= image.blocks.size()) return Status::invalid_argument("model READ bounds");
        const auto &block = image.blocks[(sector - origin - volume_offset) / 2];
        for (usize at = 0; at < 1024; ++at) out[at] = std::byte(block[at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        ++writes; if (hook_kind == 2 && writes == hook_number && hook) hook(context);
        if (fail_kind == 2 && writes == fail_number) return Status::io_error("model selected WRITE failure");
        if (sector < origin + volume_offset || (sector - origin - volume_offset) % 2 || bytes.size() != 1024 ||
            (sector - origin - volume_offset) / 2 >= image.blocks.size()) return Status::invalid_argument("model WRITE bounds");
        auto &block = image.blocks[(sector - origin - volume_offset) / 2];
        for (usize at = 0; at < 1024; ++at) block[at] = std::to_integer<u8>(bytes[at]);
        return {};
    }
    Status flush() override
    {
        ++flushes; if (hook_kind == 3 && flushes == hook_number && hook) hook(context);
        if (fail_kind == 3 && flushes == fail_number) return Status::io_error("model selected FLUSH failure");
        durable = image.blocks;
        if (barriers)
        {
            const usize stage = recovery ? flushes + 3 : flushes;
            const auto start = jbd2::detail::be32(durable[1000], 28);
            if (stage <= 2)
            {
                CHECK(!start); for (u32 home = 0; home < 4; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.before[home]);
            }
            if (stage == 3)
            { CHECK(start == 1); for (u32 home = 0; home < 4; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.before[home]); }
            if (stage == 4) { CHECK(start == 1 && durable[expected_plan.targets[0]] == expected_plan.after[0]); }
            if (stage == 5 || stage == 6)
            {
                CHECK(start == (stage == 5 ? 1U : 0U));
                for (u32 home = 0; home < 4; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.after[home]);
            }
        }
        return {};
    }
};
struct Mutation
{
    Transaction &value; Session &foreign; u32 kind{};
    static void apply(void *raw)
    {
        auto &m = *static_cast<Mutation *>(raw);
        if (m.kind == 0) m.value.original_ = &m.foreign;
        if (m.kind == 1) m.value.original_seal_ = &m.foreign;
        if (m.kind == 2) ++m.value.control_.reads;
        if (m.kind == 3) ++m.value.progress_.writes;
        if (m.kind == 4) ++m.value.binding_.journal_sequence;
        if (m.kind == 5) ++m.value.request_.generation;
        if (m.kind == 6) ++m.value.reserved_.writes;
        if (m.kind == 7) ++m.value.initial_.reads;
        if (m.kind == 8) m.value.anchor_.first_ = &m.foreign;
        if (m.kind == 9) m.value.images_.plan.after[0][99] ^= 1;
        if (m.kind == 10) m.value.images_seal_.journal.commit[57] ^= 1;
        if (m.kind == 11) ++m.value.flight_seal_.flushes;
    }
};
struct Counts { usize reads{}, writes{}, flushes{}; };
Counts run(bool recovery, u8 mask = 0, u32 count = 3, bool cross = false, u64 origin = 11,
           u32 fail_kind = 0, usize fail_number = 0, u32 change = 99, u32 hook_kind = 0,
           u32 sequence = 71, bool exhausted_version = false, bool readonly = false, usize hook_number = 1)
{
    Image image(count, cross); head(image, sequence);
    if (exhausted_version)
    { put32(image.inode(13), 36, ~u32{0}); put32(image.inode(13), 152, ~u32{0}); stamp_inode(image.inode(13), 13, image.f.seed); }
    if (count) { if (recovery) active(image, mask); else expected(image); }
    image.f.binding.layout.first_sector = 7; before = durable = image.blocks;
    Device device(image, origin), other_device(image, origin); device.recovery = recovery; device.barriers = count && !fail_kind && change == 99;
    Registry registry; Session session, foreign;
    const auto key = registry.register_device(device, 41), other = registry.register_device(other_device, 42); CHECK(key && other);
    CHECK(registry.acquire(key.value(), {origin, device.sectors - origin}, readonly ? block::SessionAccess::readonly : block::SessionAccess::writable, session));
    CHECK(registry.acquire(other.value(), {origin, other_device.sectors - origin}, block::SessionAccess::writable, foreign));
    auto *value = ::new (arena.data()) Transaction{};
    device.fail_kind = fail_kind; device.fail_number = fail_number;
    Mutation mutation{*value, foreign, change};
    device.context = &mutation; device.hook = Mutation::apply; device.hook_kind = hook_kind; device.hook_number = hook_number;
    Status status = recovery ? value->prepare_recovery(session, image.f.binding) : value->prepare(session, image.f.binding, image.update());
    if (status && count)
    {
        if (!recovery) status = value->publish();
        if (status) status = value->checkpoint();
    }
    const auto report = value->report(); const auto actual = session.report(), untouched = foreign.report(); CHECK(actual && untouched);
    CHECK(reclaim_history::same_session(report.observed, actual.value()));
    CHECK(untouched.value().state == block::DeviceState::owned && !untouched.value().reads && !untouched.value().writes && !untouched.value().flushes);
    CHECK(!other_device.reads && !other_device.writes && !other_device.flushes);
    if (fail_kind || change != 99)
    {
        CHECK(!status && report.phase == Phase::failed && actual.value().state == block::DeviceState::quarantined && !actual.value().in_flight);
        if (fail_kind)
        {
            CHECK(actual.value().first_error == StatusCode::io_error);
            const bool delegated = recovery && value->recovery_preview_.control_.phase == Ext4ZeroTruncateSessionPreviewPhase::failed;
            if (delegated)
            { CHECK(!report.reads && !report.completed_reads && actual.value().reads); }
            else
            {
                CHECK(!report.dispatch_pending);
                CHECK(report.reads == actual.value().reads && report.completed_reads == actual.value().completed_reads);
                CHECK(report.writes == actual.value().writes && report.completed_writes == actual.value().completed_writes);
                CHECK(report.flushes == actual.value().flushes && report.completed_flushes == actual.value().completed_flushes);
            }
            const auto &control = value->control_;
            CHECK(report.completed_writes == (recovery ? 0 : control.journal_cursor) +
                Transaction::population(u8(control.home_mask & ~value->images_.initial_home_mask)) + (control.cleanup ? 1 : 0));
            CHECK(report.completed_flushes == control.publication_flushes + control.checkpoint_flushes);
            ++faults;
        }
        else ++changes;
    }
    else
    {
        CHECK(status && report.phase == Phase::complete && !actual.value().pending_writes && !actual.value().in_flight);
        CHECK(report.reads == actual.value().reads && report.writes == actual.value().writes && report.flushes == actual.value().flushes);
        CHECK(report.reads <= report.reserved.reads && report.writes <= report.reserved.writes && report.flushes <= report.reserved.flushes);
        if (count)
        {
            CHECK(report.home_after_mask == 15 && report.released_count == count && report.final_sequence == 73);
            CHECK(device.writes == (recovery ? 5 - Transaction::population(mask) : 13));
            CHECK(device.flushes == (recovery ? 3 : 6)); CHECK(durable == image.blocks);
            for (u32 home = 0; home < 4; ++home) CHECK(image.blocks[expected_plan.targets[home]] == expected_plan.after[home]);
            CHECK(image.blocks[1000] == expected_journal.empty_head);
            for (u32 block = 0; block < image.blocks.size(); ++block)
            {
                bool allowed = block >= 1000 && block < 1008;
                for (u32 target : expected_plan.targets) allowed |= block == target;
                if (!allowed) CHECK(image.blocks[block] == before[block]);
            }
        }
        else { CHECK(report.noop && report.final_sequence == sequence && !device.writes && !device.flushes && image.blocks == before); }
        ++positives;
    }
    const auto totals = Counts{device.reads, device.writes, device.flushes};
    const auto paused = session.report().value();
    CHECK(!value->prepare(session, image.f.binding, image.update())); CHECK(!value->publish()); CHECK(!value->checkpoint());
    CHECK(reclaim_history::same_session(paused, session.report().value())); ++refusals;
    value->~Transaction(); if (status) CHECK(session.close()); CHECK(foreign.close());
    return totals;
}
void admission_models()
{
    for (u32 variant = 0; variant < 13; ++variant)
    {
        const bool noop = variant >= 9;
        Image image(noop ? 0 : 3); head(image); image.f.binding.layout.first_sector = 7;
        Device device(image, 11); Registry registry; Session session;
        const auto key = registry.register_device(device, 41, variant == 4 ? 512 : 65536); CHECK(key);
        CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, variant == 5 ? block::SessionAccess::readonly : block::SessionAccess::writable, session));
        auto *value = ::new (arena.data()) Transaction{};
        auto &stored = registry.find(session)->report;
        if (variant == 0) image.f.binding.journal_sequence = 0xfffffffdU;
        if (variant == 1) image.f.binding.metadata_count = 3;
        if (variant == 2) image.f.binding.layout.block_size = 512;
        if (variant == 3) image.f.binding.layout.filesystem_blocks = device.sectors;
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(stored, 1, noop ? Route::noop : Route::fresh, 71); CHECK(capacity);
        if (variant == 6) stored.reads = stored.completed_reads = ~usize{0} - capacity.value().reads + 1;
        if (variant == 7) stored.completed_writes = 1;
        if (variant == 8) stored.pending_writes = true;
        if (variant == 9 || variant == 10)
        { stored.writes = stored.completed_writes = stored.flushes = stored.completed_flushes = ~usize{0}; }
        if (variant == 10) stored.reads = stored.completed_reads = ~usize{0} - capacity.value().reads;
        if (variant == 11)
        {
            value->binding_ = image.f.binding;
            CHECK(!value->prepare(session, value->binding_, image.update()));
            CHECK(!value->attempted_ && !device.reads); ++refusals;
        }
        const auto initial = session.report().value(); before = image.blocks;
        auto copy = value->report(); copy.reserved.reads = ~usize{0}; copy.phase = Phase::published;
        CHECK(copy.phase == Phase::published && copy.reserved.reads == ~usize{0});
        CHECK(value->report().phase == Phase::unused && !value->report().reserved.reads);
        const auto status = value->prepare(session, image.f.binding, image.update());
        const auto actual = session.report().value(); const auto report = value->report();
        CHECK(reclaim_history::same_session(report.observed, actual));
        CHECK(!device.writes && !device.flushes && before == image.blocks);
        if (variant < 9)
        {
            CHECK(!status && report.phase == Phase::failed && !device.reads);
            CHECK(reclaim_history::same_session(initial, actual)); ++refusals;
        }
        else
        {
            CHECK(status && report.phase == Phase::complete && report.noop);
            CHECK(actual.reads - initial.reads == report.reads && actual.completed_reads - initial.completed_reads == report.completed_reads);
            CHECK(actual.writes == initial.writes && actual.flushes == initial.flushes); ++positives;
        }
        value->~Transaction();
        if (variant == 8) stored.pending_writes = false;
        CHECK(session.close());
    }
}
void budget_models()
{
    block::SessionReport initial{}; initial.state = block::DeviceState::owned;
    initial.identity = initial.registration_generation = initial.session_serial = 1;
    for (u32 group = 1; group <= 32; ++group) for (Route route : {Route::fresh, Route::recovery, Route::noop})
    {
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(initial, group, route, 71); CHECK(capacity);
        const auto value = capacity.value();
        CHECK(value.reads == (route == Route::fresh ? 5849 + 2124 * group : route == Route::recovery ? 4351 + 1593 * group : 1446 + 531 * group));
        CHECK(value.writes == (route == Route::fresh ? 13U : route == Route::recovery ? 5U : 0U));
        for (u32 field = 0; field < 3; ++field)
        {
            auto boundary = initial; const usize count = field == 0 ? value.reads : field == 1 ? value.writes : value.flushes;
            auto &attempts = field == 0 ? boundary.reads : field == 1 ? boundary.writes : boundary.flushes;
            auto &completed = field == 0 ? boundary.completed_reads : field == 1 ? boundary.completed_writes : boundary.completed_flushes;
            attempts = completed = ~usize{0} - count;
            CHECK(Ext4ZeroTruncateTransactionBudget::capacity(boundary, group, route, 71)); ++budgets;
            if (count) { ++attempts; ++completed; CHECK(!Ext4ZeroTruncateTransactionBudget::capacity(boundary, group, route, 71)); ++budgets; }
        }
    }
    CHECK(Ext4ZeroTruncateTransactionBudget::capacity(initial, 1, Route::noop, ~u32{0}));
    CHECK(!Ext4ZeroTruncateTransactionBudget::capacity(initial, 1, Route::fresh, 0xfffffffdU));
    CHECK(!Ext4ZeroTruncateTransactionBudget::capacity(initial, 1, Route::recovery, 0xfffffffeU));
    CHECK(!Ext4ZeroTruncateTransactionBudget::capacity<u8>(initial, 1, Route::noop, 1));
    CHECK(Ext4ZeroTruncateTransactionBudget::capacity<u16>(initial, 28, Route::fresh, 1));
    CHECK(!Ext4ZeroTruncateTransactionBudget::capacity<u16>(initial, 29, Route::fresh, 1)); budgets += 6;
}
} // namespace
int main()
{
    budget_models();
    admission_models();
    for (bool cross : {false, true}) run(false, 0, 0, cross, 11, 0, 0, 99, 0, ~u32{0}, true, true);
    for (bool cross : {false, true}) for (u32 count : {0U, 1U, 3U, 16U}) for (u64 origin : {0ULL, 11ULL}) run(false, 0, count, cross, origin);
    for (bool cross : {false, true}) for (u8 mask = 0; mask < 16; ++mask) run(true, mask, 3, cross);
    const auto fresh = run(false), recovery = run(true);
    for (bool cold : {false, true})
    {
        const auto counts = cold ? recovery : fresh;
        for (u32 kind = 1; kind <= 3; ++kind)
        {
            const usize limit = kind == 1 ? counts.reads : kind == 2 ? counts.writes : counts.flushes;
            for (usize at = 1; at <= limit; ++at) run(cold, 0, 3, false, 11, kind, at);
        }
    }
    for (u32 kind = 0; kind < 12; ++kind)
    {
        if (kind < 9 || kind == 11) run(false, 0, 3, false, 11, 0, 0, kind, 1);
        run(false, 0, 3, false, 11, 0, 0, kind, 2);
        run(false, 0, 3, false, 11, 0, 0, kind, 3);
    }
    for (bool cold : {false, true}) for (u32 kind = 0; kind < 12; ++kind)
        run(cold, 0, 3, false, 11, 0, 0, kind, 1, 71, false, false, cold ? recovery.reads : fresh.reads);
    std::printf("PASS hosted private zero-truncate transaction positive=%zu refusal=%zu faults=%zu changes=%zu budget=%zu fresh_reads=%zu recovery_reads=%zu\n",
        positives, refusals, faults, changes, budgets, fresh.reads, recovery.reads);
    std::printf("HOSTED transaction_size=%zu transaction_alignment=%zu native_stack_chain_qualified=no\n", sizeof(Transaction), alignof(Transaction));
    return 0;
}
