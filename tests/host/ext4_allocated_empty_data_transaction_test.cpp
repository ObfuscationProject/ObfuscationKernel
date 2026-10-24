#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_allocated_empty_data_transaction.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>

// A narrow hosted owner friend enters the production private transaction API.
// No production access modifier, private Session layout or header is changed.
namespace ok::fs
{
template <usize Services, usize Cache> class Ext4OwnedTransaction
{
  public:
    using Registry = block::BlockSessions<Services>;
    using Session = typename Registry::Session;
    using Transaction = Ext4AllocatedEmptyDataTransaction<Session>;
    static Status prepare(Transaction &value, Session &session, const Ext4RecoveryInfo &binding,
                          Ext4AllocatedEmptyDataUpdate update, u32 offset, const Ext4FirstDataBlock &data)
    { return value.prepare(session, binding, update, offset, data); }
    static Status recover(Transaction &value, Session &session, const Ext4RecoveryInfo &binding)
    { return value.prepare_recovery(session, binding); }
    static Status publish(Transaction &value) { return value.publish(); }
    static Status checkpoint(Transaction &value) { return value.checkpoint(); }
    static const Ext4RecoveryInfo &binding_alias(const Transaction &value) { return value.binding_; }
    static const Ext4FirstDataBlock &data_alias(const Transaction &value) { return value.data_request_; }
    static bool delegated_failure(const Transaction &value)
    { return value.recovery_preview_.report().phase == Ext4AllocatedEmptyDataSessionPreviewPhase::failed; }
    static bool completion_credit_exact(const Transaction &value, bool recovery)
    {
        const auto &c = value.control_;
        u32 homes{};
        const u8 gained = u8(c.home_mask & ~value.images_.initial_home_mask);
        for (u32 bit = 0; bit < 4; ++bit) if (gained & (1U << bit)) ++homes;
        return c.completed_writes == (recovery ? 0 : c.journal_cursor) + homes +
                   (c.cleanup ? 1 : 0) + (c.data_written ? 1 : 0) &&
               c.completed_flushes == c.publication_flushes + c.checkpoint_flushes + (c.data_flushed ? 1 : 0);
    }
    static void mutate(Transaction &value, Session &foreign, u32 field)
    {
        switch (field)
        {
        case 0: value.original_ = &foreign; break;
        case 1: value.original_seal_ = &foreign; break;
        case 2: ++value.control_.reads; break;
        case 3: ++value.sealed_.completed_reads; break;
        case 4: ++value.binding_.metadata[127].count; break;
        case 5: ++value.binding_seal_.journal_sequence; break;
        case 6: ++value.request_.generation; break;
        case 7: value.data_request_[99] ^= 1; break;
        case 8: value.data_request_seal_[511] ^= 1; break;
        case 9: ++value.reserved_.reads; break;
        case 10: ++value.progress_.reads; break;
        case 11: ++value.progress_seal_.completed_writes; break;
        case 12: ++value.initial_.session_serial; break;
        case 13: ++value.initial_seal_.registration_generation; break;
        case 14: ++value.geometry_.block_count; break;
        case 15: ++value.bounds_seal_.count; break;
        case 16: ++value.flight_.reads; break;
        case 17: ++value.flight_seal_.completed_reads; break;
        case 18: value.images_.plan.after[3][97] ^= 1; break;
        case 19: value.images_seal_.original_data[881] ^= 1; break;
        case 20: ++value.control_.journal_cursor; break;
        case 21: ++value.control_.publication_flushes; break;
        case 22: ++value.control_.checkpoint_flushes; break;
        case 23: value.control_.home_mask ^= 8; break;
        case 24: value.control_.data_written = !value.control_.data_written; break;
        case 25: value.control_.data_flushed = !value.control_.data_flushed; break;
        }
    }
};
}

using namespace zero_preview_fixture;
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "allocated empty transaction:%d: %s case=%zu\n", __LINE__, #v, cases); std::abort(); } } while (false)
namespace
{
using Owner = Ext4OwnedTransaction<2, 8>;
using Registry = Owner::Registry;
using Session = Owner::Session;
using Transaction = Owner::Transaction;
using Phase = Ext4AllocatedEmptyDataTransactionPhase;
using Route = Ext4AllocatedEmptyDataRoute;
static_assert(!std::is_copy_constructible_v<Transaction> && !std::is_move_constructible_v<Transaction>);
alignas(Transaction) std::array<std::byte, sizeof(Transaction)> arena{};
std::array<Block, 8705> before{}, durable{};
Ext4AllocatedEmptyDataScratch planning{};
Ext4AllocatedEmptyDataPlan expected_plan{};
Ext4AllocatedEmptyDataJournalFormatScratch formatting{};
Ext4AllocatedEmptyDataJournalFormat expected_journal{};
Block data{};
usize cases{}, positives{}, faults{}, partial_faults{}, changes{}, aliases{}, refusals{}, budgets{}, interstage{}, max_reads{};
u32 population(u8 mask) { u32 count{}; for (u32 bit = 0; bit < 4; ++bit) if (mask & (1U << bit)) ++count; return count; }
void be32(Block &bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - byte * 8)); }
void media(Image &image, bool recovery, u8 mask, u32 size, u32 gap)
{
    auto &head = image.blocks[1000]; head = {};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3 | jbd2::feature_revoke); be32(head, 64, 1); head[80] = 4;
    for (usize at = 0; at < 16; ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = 70;
    for (u32 record = 1; record < 8; ++record)
        for (usize at = 0; at < 1024; ++at) image.blocks[1000 + record][at] = u8(record * 23 + at * 7);
    const u32 physical = image.f.cross ? image.f.physical : 70;
    for (usize at = 0; at < 1024; ++at) image.blocks[physical][at] = u8(17 + at * 19);
    data = {};
    for (usize at = gap; at < size; ++at) data[at] = u8(1 + (at * 17) % 251);
    const Ext4AllocatedEmptyDataUpdate update{13, 9, size, 1700000222, 123456789};
    const Ext4AllocatedEmptyDataSource source{image.f.group, physical, 8, image.blocks[1], image.blocks[2],
        image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, update, source, planning, expected_plan));
    CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(image.f.binding, expected_plan, head,
        image.blocks[4], formatting, expected_journal));
    if (recovery)
    {
        image.blocks[1000] = expected_journal.head; image.blocks[1001] = expected_journal.descriptor;
        for (u32 at = 0; at < 4; ++at) image.blocks[1002 + at] = expected_journal.payloads[at];
        image.blocks[1006] = expected_journal.commit; image.blocks[1007] = expected_journal.end;
        for (u32 home = 0; home < 4; ++home)
            image.blocks[expected_plan.targets[home]] = mask & (1U << home) ? expected_plan.after[home] : expected_plan.before[home];
        image.blocks[physical] = data;
        image.f.binding.empty_log = false; image.f.binding.journal_sequence = expected_journal.active_sequence;
    }
}
struct Trace { u32 kind{}, block{}; Phase phase{}; };
struct Device final : driver::BlockDevice
{
    Image &image; u64 origin{}, offset{}, sectors{};
    Transaction *value{}; Session *foreign{};
    usize reads{}, writes{}, flushes{}, total{}, fail_at{}, hook_at{};
    mutable usize geometry_calls{};
    u32 mutation{99}, hook_point{};
    bool partial{}, fired{}, verify_barriers{}, recovery{};
    std::array<Trace, 1024> traces{}; usize trace_count{};
    Device(Image &source, u64 partition, u64 volume)
        : image(source), origin(partition), offset(volume), sectors(partition + volume + source.blocks.size() * 2 + 8) {}
    void inject() { CHECK(value && foreign); Owner::mutate(*value, *foreign, mutation); fired = true; }
    driver::BlockGeometry geometry() const override
    {
        ++geometry_calls;
        if (!fired && mutation != 99 && hook_point == 1 && geometry_calls == 2 * hook_at - 1)
            const_cast<Device *>(this)->inject();
        if (!fired && mutation != 99 && hook_point == 4 && geometry_calls == 2 * hook_at)
            const_cast<Device *>(this)->inject();
        return {sectors, 512, true};
    }
    u32 block(u64 sector, usize bytes)
    {
        CHECK(sector >= origin + offset && !((sector - origin - offset) % 2) && bytes == 1024);
        const auto number = (sector - origin - offset) / 2; CHECK(number < image.blocks.size()); return u32(number);
    }
    void enter(u32 kind, u32 number)
    {
        ++total;
        if (value) { CHECK(trace_count < traces.size()); traces[trace_count++] = {kind, number, value->report().phase}; }
        if (!fired && mutation != 99 && hook_point == 2 && total == hook_at) inject();
    }
    Status leave()
    {
        if (!fired && mutation != 99 && hook_point == 3 && total == hook_at) inject();
        return total == fail_at ? Status::io_error("ordered allocated empty selected transport failure") : Status{};
    }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; const auto number = block(sector, out.size()); enter(1, number);
        const usize copied = total == fail_at ? (partial ? 511 : 0) : 1024;
        for (usize at = 0; at < copied; ++at) out[at] = std::byte(image.blocks[number][at]);
        return leave();
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        ++writes; const auto number = block(sector, in.size()); enter(2, number);
        const usize copied = total == fail_at ? (partial ? 511 : 0) : 1024;
        for (usize at = 0; at < copied; ++at) image.blocks[number][at] = std::to_integer<u8>(in[at]);
        return leave();
    }
    Status flush() override
    {
        ++flushes; enter(3, 0);
        if (total != fail_at)
        {
            durable = image.blocks;
            if (verify_barriers)
            {
                const usize stage = recovery ? flushes + 4 : flushes;
                CHECK(durable[expected_plan.physical] == data);
                const auto start = jbd2::detail::be32(durable[1000], 28);
                if (stage <= 3)
                {
                    CHECK(!start);
                    for (u32 home = 0; home < 4; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.before[home]);
                }
                if (stage == 4)
                { CHECK(start == 1); for (u32 home = 0; home < 4; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.before[home]); }
                if (stage >= 5)
                {
                    CHECK(start == (stage == 7 ? 0U : 1U));
                    for (u32 home = 0; home < 3; ++home) CHECK(durable[expected_plan.targets[home]] == expected_plan.after[home]);
                    if (stage >= 6) CHECK(durable[expected_plan.targets[3]] == expected_plan.after[3]);
                }
            }
        }
        return leave();
    }
};
struct Scenario
{
    bool recovery{}, cross{}; u8 mask{}; u32 size{73}, gap{}; u64 origin{19}, offset{7};
    usize failure{}, hook{}; bool partial{}; u32 mutation{99}, point{}, stage{99};
};
struct Counts { usize reads{}, writes{}, flushes{}, operations{}, first_write{}, first_flush{}; };
Counts run(Scenario input)
{
    ++cases; Image image(0, input.cross); media(image, input.recovery, input.mask, input.size, input.gap);
    image.f.binding.layout.first_sector = input.offset; before = durable = image.blocks;
    Device device(image, input.origin, input.offset), decoy(image, input.origin, input.offset);
    Registry registry; Session session, foreign;
    const auto key = registry.register_device(device, 71), other = registry.register_device(decoy, 79); CHECK(key && other);
    const auto bounds = block::SessionBounds{input.origin, device.sectors - input.origin};
    CHECK(registry.acquire(key.value(), bounds, block::SessionAccess::writable, session));
    CHECK(registry.acquire(other.value(), bounds, block::SessionAccess::writable, foreign));
    Block prefix{};
    CHECK(session.read_blocks(input.offset + 2 * 1000, std::as_writable_bytes(std::span{prefix})));
    CHECK(session.write_blocks(input.offset + 2 * 1000, std::as_bytes(std::span{prefix})));
    CHECK(session.flush());
    const auto initial = session.report().value(), untouched_before = foreign.report().value();
    device.reads = device.writes = device.flushes = device.total = device.geometry_calls = 0;
    auto *value = ::new (arena.data()) Transaction{};
    device.value = value; device.foreign = &foreign; device.fail_at = input.failure; device.partial = input.partial;
    device.hook_at = input.hook; device.hook_point = input.point; device.mutation = input.mutation;
    device.verify_barriers = !input.failure && input.mutation == 99 && input.stage == 99; device.recovery = input.recovery;
    const Ext4AllocatedEmptyDataUpdate update{13, 9, input.size, 1700000222, 123456789};
    auto status = input.recovery ? Owner::recover(*value, session, image.f.binding) :
        Owner::prepare(*value, session, image.f.binding, update, input.gap, data);
    if (status && input.stage != 99)
    {
        if (input.stage < 26) Owner::mutate(*value, foreign, input.stage);
        else if (input.stage == 26) image.blocks[expected_plan.physical][901] ^= 1;
        else image.blocks[1007][991] ^= 1;
        status = input.recovery ? Owner::checkpoint(*value) : Owner::publish(*value); ++interstage;
    }
    else if (status)
    {
        if (!input.recovery) status = Owner::publish(*value);
        if (status) status = Owner::checkpoint(*value);
    }
    const auto report = value->report();
    const auto actual = session.report().value(), untouched = foreign.report().value();
    CHECK(reclaim_history::same_session(report.observed, actual));
    CHECK(reclaim_history::same_session(untouched, untouched_before) && !decoy.reads && !decoy.writes && !decoy.flushes);
    CHECK(!actual.in_flight);
    if (input.failure || input.mutation != 99 || input.stage != 99)
    {
        CHECK(!status && report.phase == Phase::failed && actual.state == block::DeviceState::quarantined);
        if (input.failure)
        {
            CHECK(device.total == input.failure && actual.first_error == StatusCode::io_error && !report.dispatch_pending);
            if (input.recovery && Owner::delegated_failure(*value)) CHECK(!report.reads && !report.completed_reads);
            else
            {
                CHECK(report.reads == actual.reads - initial.reads && report.completed_reads == actual.completed_reads - initial.completed_reads);
                CHECK(report.writes == actual.writes - initial.writes && report.completed_writes == actual.completed_writes - initial.completed_writes);
                CHECK(report.flushes == actual.flushes - initial.flushes && report.completed_flushes == actual.completed_flushes - initial.completed_flushes);
            }
            CHECK(Owner::completion_credit_exact(*value, input.recovery)); ++faults; if (input.partial) ++partial_faults;
        }
        else { if (input.mutation != 99) CHECK(device.fired); ++changes; }
    }
    else
    {
        CHECK(status && report.phase == Phase::complete && !actual.pending_writes);
        CHECK(report.reads == device.reads && report.writes == device.writes && report.flushes == device.flushes);
        CHECK(report.home_after_mask == 15 && report.physical == expected_plan.physical && report.size == input.size && report.final_sequence == 72);
        CHECK(report.data_written == !input.recovery && report.data_flushed == !input.recovery);
        CHECK(device.writes == (input.recovery ? 5 - population(input.mask) : 14)); CHECK(device.flushes == (input.recovery ? 3 : 7));
        CHECK(report.reads <= report.reserved.reads && report.writes <= report.reserved.writes && report.flushes <= report.reserved.flushes);
        CHECK(image.blocks[expected_plan.physical] == data && image.blocks[1000] == expected_journal.empty_head && durable == image.blocks);
        for (u32 home = 0; home < 4; ++home) CHECK(image.blocks[expected_plan.targets[home]] == expected_plan.after[home]);
        for (u32 number = 0; number < image.blocks.size(); ++number)
        {
            bool allowed = number == expected_plan.physical || (number >= 1000 && number < 1008);
            for (u32 target : expected_plan.targets) allowed |= target == number;
            if (!allowed) CHECK(image.blocks[number] == before[number]);
        }
        usize publication{}, allocation{}, cleanup{}; bool allocation_barrier{}, inode_barrier{};
        constexpr std::array<u32, 8> order{1001, 1002, 1003, 1004, 1005, 1007, 1006, 1000};
        for (usize at = 0; at < device.trace_count; ++at)
        {
            const auto &trace = device.traces[at];
            if (trace.kind == 2 && trace.phase == Phase::publishing) { CHECK(trace.block == order[publication]); ++publication; }
            if (trace.kind == 2 && trace.phase == Phase::allocations) { CHECK(!allocation_barrier); ++allocation; }
            if (trace.kind == 3 && trace.phase == Phase::allocations) allocation_barrier = true;
            if (trace.kind == 2 && trace.phase == Phase::inode) CHECK(allocation_barrier && trace.block == expected_plan.targets[3]);
            if (trace.kind == 3 && trace.phase == Phase::inode) { CHECK(allocation_barrier); inode_barrier = true; }
            if (trace.kind == 2 && trace.phase == Phase::cleanup) { CHECK(inode_barrier && trace.block == 1000); ++cleanup; }
        }
        CHECK(publication == (input.recovery ? 0 : 8) && allocation == 3 - population(u8(input.mask & 7)) && cleanup == 1);
        CHECK(Owner::completion_credit_exact(*value, input.recovery)); ++positives;
    }
    Counts counts{device.reads, device.writes, device.flushes, device.total};
    for (usize at = 0; at < device.trace_count; ++at)
    {
        if (device.traces[at].kind == 2 && !counts.first_write) counts.first_write = at + 1;
        if (device.traces[at].kind == 3 && !counts.first_flush) counts.first_flush = at + 1;
    }
    if (device.reads > max_reads) max_reads = device.reads;
    const auto stopped = session.report().value(); const auto operations = device.total;
    auto inert = value->report(); inert.phase = Phase::prepared; inert.reserved.writes = ~usize{0};
    CHECK(value->report().phase != inert.phase && value->report().reserved.writes != inert.reserved.writes);
    CHECK(!Owner::prepare(*value, session, image.f.binding, update, input.gap, data));
    CHECK(!Owner::recover(*value, session, image.f.binding) && !Owner::publish(*value) && !Owner::checkpoint(*value));
    CHECK(device.total == operations && reclaim_history::same_session(stopped, session.report().value())); ++refusals;
    value->~Transaction(); if (status) CHECK(session.close()); CHECK(foreign.close()); return counts;
}
void budget_models()
{
    block::SessionReport initial{}; initial.state = block::DeviceState::owned;
    initial.identity = initial.registration_generation = initial.session_serial = 1;
    for (u32 group = 1; group <= 32; ++group) for (Route route : {Route::fresh, Route::recovery})
    {
        const auto capacity = Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, group, route, 70); CHECK(capacity);
        const auto value = capacity.value();
        CHECK(value.reads == (route == Route::fresh ? 5884 + 2125 * group : 4366 + 1593 * group));
        CHECK(value.writes == (route == Route::fresh ? 14U : 5U) && value.flushes == (route == Route::fresh ? 7U : 3U)); ++budgets;
        for (u32 field = 0; field < 3; ++field)
        {
            auto v = initial; const usize count = field == 0 ? value.reads : field == 1 ? value.writes : value.flushes;
            auto &attempt = field == 0 ? v.reads : field == 1 ? v.writes : v.flushes;
            auto &complete = field == 0 ? v.completed_reads : field == 1 ? v.completed_writes : v.completed_flushes;
            attempt = complete = ~usize{0} - count; CHECK(Ext4AllocatedEmptyDataTransactionBudget::capacity(v, group, route, 70)); ++budgets;
            ++attempt; ++complete;
            const auto overflow = Ext4AllocatedEmptyDataTransactionBudget::capacity(v, group, route, 70);
            CHECK(!overflow && overflow.status().code() == StatusCode::overflow); ++budgets;
            --attempt; --complete;
            ++attempt; CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(v, group, route, 70)); ++budgets;
            --attempt; ++complete; CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(v, group, route, 70)); ++budgets;
        }
    }
    for (u32 group : {0U, 33U}) { CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, group, Route::fresh, 70)); ++budgets; }
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, 1, Route(255), 70));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, 1, Route::fresh, 0));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, 1, Route::fresh, 0xfffffffdU));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, 1, Route::recovery, 1));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity(initial, 1, Route::recovery, 0xfffffffeU));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity<u8>(initial, 1, Route::fresh, 70));
    CHECK(Ext4AllocatedEmptyDataTransactionBudget::capacity<u16>(initial, 28, Route::fresh, 70));
    CHECK(!Ext4AllocatedEmptyDataTransactionBudget::capacity<u16>(initial, 29, Route::fresh, 70)); budgets += 8;
}
void alias_and_admission()
{
    for (u32 variant = 0; variant < 13; ++variant)
    {
        ++cases; Image image(0); media(image, false, 0, 73, 0); Device device(image, 19, 7);
        image.f.binding.layout.first_sector = 7; Registry registry; Session session;
        const auto key = registry.register_device(device, 11, variant == 9 ? 512 : 65536); CHECK(key);
        CHECK(registry.acquire(key.value(), {19, device.sectors - 19}, variant == 10 ? block::SessionAccess::readonly : block::SessionAccess::writable, session));
        auto *value = ::new (arena.data()) Transaction{};
        Ext4AllocatedEmptyDataUpdate update{13, 9, 73, 1700000222, 123456789}; u32 offset{};
        if (variant == 0) CHECK(!Owner::prepare(*value, session, Owner::binding_alias(*value), update, 0, data));
        if (variant == 1) CHECK(!Owner::prepare(*value, session, image.f.binding, update, 0, Owner::data_alias(*value)));
        if (variant < 2)
        {
            CHECK(value->report().phase == Phase::unused && !device.total); ++aliases;
            CHECK(Owner::prepare(*value, session, image.f.binding, update, 0, data));
            const auto observed = session.report().value(); CHECK(!device.writes && !device.flushes && observed.reads); value->~Transaction();
            CHECK(session.report().value().state == block::DeviceState::quarantined); continue;
        }
        if (variant == 2) update.size = 0;
        if (variant == 3) update.size = 1025;
        if (variant == 4) offset = 73;
        if (variant == 5) update.nanoseconds = 1000000000;
        if (variant == 6) data[900] = 1;
        if (variant == 7) image.f.binding.metadata_count = 3;
        if (variant == 8) image.f.binding.journal_sequence = 0xfffffffdU;
        if (variant == 11) image.f.binding.layout.block_size = 2048;
        if (variant == 12) image.f.binding.needs_recovery = false;
        CHECK(!Owner::prepare(*value, session, image.f.binding, update, offset, data));
        CHECK(!device.total && !device.writes && !device.flushes); ++refusals; value->~Transaction(); CHECK(session.close());
    }
}
}
int main()
{
    budget_models(); alias_and_admission();
    for (bool cross : {false, true}) for (u32 size : {1U, 73U, 1024U}) for (u64 origin : {0ULL, 19ULL})
    {
        for (u32 gap : {0U, size - 1}) { Scenario s; s.cross = cross; s.size = size; s.gap = gap; s.origin = origin; run(s); }
        for (u8 mask = 0; mask < 16; ++mask) { Scenario s; s.recovery = true; s.cross = cross; s.size = size; s.mask = mask; s.origin = origin; run(s); }
    }
    for (bool cross : {false, true}) for (bool recovery : {false, true})
    {
        Scenario baseline; baseline.recovery = recovery; baseline.cross = cross; const auto count = run(baseline);
        for (usize at = 1; at <= count.operations; ++at) for (bool partial : {false, true})
        { auto s = baseline; s.failure = at; s.partial = partial; run(s); }
        for (usize at : {count.operations, count.first_write, count.first_flush})
            for (u32 field = 0; field < 26; ++field) for (u32 point = 1; point <= 4; ++point)
            { auto s = baseline; s.hook = at; s.mutation = field; s.point = point; run(s); }
        for (u32 stage = 0; stage < 28; ++stage)
        { if (stage == 16 || stage == 17) continue; auto s = baseline; s.stage = stage; run(s); }
        std::printf("ALLOCATED_EMPTY_TRANSACTION_TRACE cross=%u recovery=%u reads=%zu writes=%zu flushes=%zu operations=%zu first_write=%zu first_flush=%zu\n",
            u32(cross), u32(recovery), count.reads, count.writes, count.flushes, count.operations, count.first_write, count.first_flush);
    }
    std::printf("ALLOCATED_EMPTY_TRANSACTION_MODEL cases=%zu positive=%zu faults=%zu partial_faults=%zu retained=%zu aliases=%zu refusals=%zu budgets=%zu interstage=%zu max_reads=%zu source_only=yes independent_acceptance=no\n",
        cases, positives, faults, partial_faults, changes, aliases, refusals, budgets, interstage, max_reads);
    return 0;
}
