#include "ok/fs/ext4/journal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <type_traits>
#include <vector>

using namespace ok;
using namespace ok::fs::jbd2;

extern "C" u32 ext2fs_crc32c_le(u32, const void *, usize);

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

using Block = std::array<u8, 1024>;
using Sector = std::array<std::byte, 512>;
using Disk = std::map<u64, Sector>;
const std::array<u8, 16> uuid{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
std::array<Block, 10> golden{};
constexpr RecoveryLayout layout{7, 1200, 128, 1024, 1024};
const std::array<BlockRange, 1> ranges{{{32, 64}}};

void put(std::span<u8> block, usize offset, u32 value)
{
    block[offset] = u8(value >> 24);
    block[offset + 1] = u8(value >> 16);
    block[offset + 2] = u8(value >> 8);
    block[offset + 3] = u8(value);
}

void seal(std::span<u8> block, usize offset, bool superblock = false)
{
    put(block, offset, 0);
    const u32 seed = superblock ? ~u32{0} : ext2fs_crc32c_le(~u32{0}, uuid.data(), uuid.size());
    put(block, offset, ext2fs_crc32c_le(seed, block.data(), block.size()));
}

void payload_checksum(std::span<u8> descriptor, usize tag_offset, u32 sequence, std::span<const u8> payload)
{
    std::array<u8, 4> encoded{};
    put(encoded, 0, sequence);
    u32 crc = ext2fs_crc32c_le(~u32{0}, uuid.data(), uuid.size());
    crc = ext2fs_crc32c_le(crc, encoded.data(), encoded.size());
    put(descriptor, tag_offset + 12, ext2fs_crc32c_le(crc, payload.data(), payload.size()));
}

u64 sector(const RecoveryLayout &mapping, u64 block)
{
    return mapping.first_sector + block * (mapping.block_size / 512);
}

// An independent byte-copy oracle; absent sectors contain a fixed sentinel.
void store(Disk &disk, u64 first, std::span<const std::byte> bytes)
{
    for (usize index = 0; index < bytes.size(); ++index)
    {
        const u64 address = first + index / 512;
        auto [found, inserted] = disk.try_emplace(address);
        if (inserted)
            found->second.fill(std::byte{0x5a});
        found->second[index % 512] = bytes[index];
    }
}

struct Event
{
    char operation{};
    u64 sector{};
    std::vector<std::byte> bytes{};
};

class Device final : public driver::BlockDevice
{
  public:
    driver::BlockGeometry reported{layout.first_sector + layout.filesystem_blocks * 2 + 7, 512, true};
    Disk live{};
    Disk durable{};
    std::vector<Event> events{};
    usize reads{};
    usize writes{};
    usize flushes{};
    usize fail_read{};
    usize fail_write{};
    usize fail_flush{};
    usize corrupt_read{};
    usize change_geometry_after_read{};
    usize change_geometry_after_write{};
    usize change_geometry_after_flush{};
    usize failed_write_bytes{};
    usize failed_flush_sectors{};
    bool failed_write_is_durable{};
    bool flush_unsupported{};

    explicit Device(const std::array<Block, 10> &fixture = golden, RecoveryLayout mapping = layout)
    {
        reported.block_count = mapping.first_sector + mapping.filesystem_blocks * (mapping.block_size / 512) + 7;
        store(live, sector(mapping, mapping.journal_first_block), std::as_bytes(std::span(fixture[0])));
        for (usize index = 1; index < fixture.size(); ++index)
        {
            const u64 journal_block = 1008 + (14 + index - 1) % 16;
            store(live, sector(mapping, mapping.journal_first_block + journal_block),
                  std::as_bytes(std::span(fixture[index])));
        }
        durable = live;
    }

    [[nodiscard]] driver::BlockGeometry geometry() const override { return reported; }

    Status read_blocks(u64 first, std::span<std::byte> bytes) override
    {
        ++reads;
        events.push_back({'r', first, {}});
        CHECK(transfer_valid(first, bytes.size()));
        if (reads == fail_read)
        {
            // Failed reads may leave caller scratch partially changed.
            if (!bytes.empty()) bytes[0] = std::byte{0xa5};
            return Status::io_error("injected recovery read failure");
        }
        for (usize index = 0; index < bytes.size(); ++index)
        {
            const auto found = live.find(first + index / 512);
            bytes[index] = found == live.end() ? std::byte{0x5a} : found->second[index % 512];
        }
        if (reads == corrupt_read)
            bytes[600] ^= std::byte{1};
        if (reads == change_geometry_after_read)
            --reported.block_count;
        return Status::success();
    }

    Status write_blocks(u64 first, std::span<const std::byte> bytes) override
    {
        ++writes;
        events.push_back({'w', first, {bytes.begin(), bytes.end()}});
        CHECK(transfer_valid(first, bytes.size()));
        if (writes == fail_write)
        {
            const auto partial = bytes.first(std::min(failed_write_bytes, bytes.size()));
            store(live, first, partial);
            if (failed_write_is_durable)
                store(durable, first, partial);
            return Status::io_error("injected partial recovery write failure");
        }
        store(live, first, bytes);
        if (writes == change_geometry_after_write)
            reported.writable = false;
        return Status::success();
    }

    Status flush() override
    {
        ++flushes;
        events.push_back({'f', 0, {}});
        if (flush_unsupported)
            return Status::unsupported("injected unavailable flush");
        if (flushes == fail_flush)
        {
            usize persisted = 0;
            for (const auto &[address, bytes] : live)
            {
                if (persisted++ == failed_flush_sectors)
                    break;
                durable[address] = bytes;
            }
            return Status::io_error("injected partial recovery flush failure");
        }
        durable = live;
        if (flushes == change_geometry_after_flush)
            --reported.block_count;
        return Status::success();
    }

    void power_cut()
    {
        live = durable;
        fail_read = fail_write = fail_flush = corrupt_read = 0;
        reads = writes = flushes = 0;
        change_geometry_after_read = change_geometry_after_write = change_geometry_after_flush = 0;
        events.clear();
    }

  private:
    bool transfer_valid(u64 first, usize bytes) const
    {
        return bytes != 0 && bytes % reported.block_size == 0 && first < reported.block_count &&
               bytes / reported.block_size <= reported.block_count - first;
    }
};

RecoveryIo bind(Device &device, RecoveryLayout mapping = layout)
{
    auto io = RecoveryIo::open(device, mapping);
    CHECK(io);
    return io.value();
}

TargetDomain domain() { return {layout.filesystem_blocks, ranges}; }

Block restored(Block source)
{
    put(source, 0, magic);
    return source;
}

Disk golden_oracle(const Device &device)
{
    Disk expected = device.live;
    const auto escaped = restored(golden[3]);
    store(expected, sector(layout, 41), std::as_bytes(std::span(escaped)));
    store(expected, sector(layout, 42), std::as_bytes(std::span(golden[7])));
    return expected;
}

void journal_unchanged(const Device &device, const Disk &before, RecoveryLayout mapping = layout)
{
    const u64 first = sector(mapping, mapping.journal_first_block);
    const u64 end = sector(mapping, mapping.journal_first_block + mapping.journal_blocks);
    for (const auto &[address, bytes] : before)
        if (address >= first && address < end)
        {
            CHECK(device.live.at(address) == bytes);
            CHECK(device.durable.at(address) == bytes);
        }
}

std::vector<Event> write_events(const Device &device)
{
    std::vector<Event> result;
    for (const auto &event : device.events)
        if (event.operation == 'w') result.push_back(event);
    return result;
}

void success_and_repetition()
{
    static_assert(!std::is_copy_constructible_v<ReplayExecutor<>>);
    static_assert(!std::is_move_constructible_v<ReplayExecutor<>>);
    Device device;
    const auto initial = device.live;
    const auto expected = golden_oracle(device);
    auto io = bind(device);
    ReplayExecutor executor;
    CHECK(executor.run(io, uuid, domain()));
    const auto &report = executor.report();
    CHECK(report.phase == ReplayPhase::complete && report.replay_durable());
    CHECK(report.preflight_flush_succeeded && report.final_flush_succeeded);
    CHECK(report.planned_actions == 2 && report.revoked_actions == 1 && report.committed_transactions == 2);
    CHECK(report.next_sequence == 1 && !report.empty_log && !report.incomplete_tail);
    CHECK(report.write_attempts == 2 && report.completed_writes == 2 && report.destination_may_have_changed());
    CHECK(report.flush_attempts == 2 && report.completed_flushes == 2);
    CHECK(device.reads == 14 && device.writes == 2 && device.flushes == 2);
    CHECK(device.live == expected && device.durable == expected);
    CHECK(device.events.front().operation == 'f' && device.events.back().operation == 'f');
    const auto writes = write_events(device);
    CHECK(writes.size() == 2 && writes[0].sector == sector(layout, 41) && writes[1].sector == sector(layout, 42));
    const auto escaped = restored(golden[3]);
    CHECK(std::equal(writes[0].bytes.begin(), writes[0].bytes.end(), std::as_bytes(std::span(escaped)).begin()));
    CHECK(std::equal(writes[1].bytes.begin(), writes[1].bytes.end(), std::as_bytes(std::span(golden[7])).begin()));
    journal_unchanged(device, initial);
    const auto operations = device.events.size();
    CHECK(!executor.run(io, uuid, domain()) && device.events.size() == operations);
    CHECK(executor.report().replay_durable());
    // The retained journal can be replayed by a fresh owner after a restart.
    device.power_cut();
    ReplayExecutor restarted;
    CHECK(restarted.run(io, uuid, domain()));
    CHECK(device.live == expected && device.durable == expected);
    journal_unchanged(device, initial);
}

void read_failures()
{
    for (usize call = 1; call <= 14; ++call)
    {
        Device device;
        const auto initial = device.live;
        auto io = bind(device);
        device.fail_read = call;
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain()));
        const auto &report = executor.report();
        CHECK(report.phase == ReplayPhase::failed && report.status.code() == StatusCode::io_error);
        CHECK(!report.replay_durable() && !report.final_flush_succeeded);
        CHECK(device.reads == call && device.flushes == 1);
        CHECK(report.write_attempts == (call == 14 ? 1U : 0U));
        CHECK(report.completed_writes == report.write_attempts);
        CHECK(report.failed_phase == (call <= 10 ? ReplayPhase::planning :
                                      call <= 12 ? ReplayPhase::preflight : ReplayPhase::replaying));
        if (call < 14) CHECK(device.live == initial);
        CHECK(device.durable == initial);
        journal_unchanged(device, initial);
        const auto operations = device.events.size();
        CHECK(!executor.run(io, uuid, domain()) && device.events.size() == operations);
        device.power_cut();
        const auto expected = golden_oracle(device);
        ReplayExecutor retry;
        CHECK(retry.run(io, uuid, domain()));
        CHECK(device.durable == expected);
    }
    for (const auto call : {11U, 12U, 13U, 14U})
    {
        Device device;
        const auto initial = device.live;
        auto io = bind(device);
        device.corrupt_read = call;
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain()));
        CHECK(executor.report().write_attempts == (call == 14 ? 1U : 0U));
        CHECK(!executor.report().replay_durable() && device.reads == call && device.flushes == 1);
        journal_unchanged(device, initial);
    }
}

void write_and_flush_failures()
{
    for (usize call = 1; call <= 2; ++call)
        for (const usize partial : {usize{0}, usize{1}, usize{511}, usize{512}, usize{1024}})
        {
            Device device;
            const auto initial = device.live;
            const auto expected = golden_oracle(device);
            auto io = bind(device);
            device.fail_write = call;
            device.failed_write_bytes = partial;
            device.failed_write_is_durable = true;
            ReplayExecutor executor;
            CHECK(!executor.run(io, uuid, domain()));
            const auto &report = executor.report();
            CHECK(report.failed_phase == ReplayPhase::replaying && report.write_attempts == call);
            CHECK(report.completed_writes == call - 1 && report.destination_may_have_changed());
            CHECK(!report.replay_durable() && device.writes == call && device.flushes == 1);
            journal_unchanged(device, initial);
            device.power_cut();
            ReplayExecutor retry;
            CHECK(retry.run(io, uuid, domain()));
            CHECK(device.live == expected && device.durable == expected);
        }
    for (const auto call : {1U, 2U})
    {
        Device device;
        const auto initial = device.live;
        const auto expected = golden_oracle(device);
        auto io = bind(device);
        device.fail_flush = call;
        device.failed_flush_sectors = 1;
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain()));
        const auto &report = executor.report();
        CHECK(report.failed_phase == (call == 1 ? ReplayPhase::validating : ReplayPhase::flushing));
        CHECK(report.flush_attempts == call && report.completed_flushes == call - 1);
        CHECK(report.write_attempts == (call == 1 ? 0U : 2U));
        CHECK(!report.final_flush_succeeded && !report.replay_durable());
        CHECK(device.flushes == call);
        journal_unchanged(device, initial);
        device.power_cut();
        ReplayExecutor retry;
        CHECK(retry.run(io, uuid, domain()));
        CHECK(device.durable == expected);
    }
    Device unavailable;
    auto io = bind(unavailable);
    unavailable.flush_unsupported = true;
    ReplayExecutor executor;
    CHECK(!executor.run(io, uuid, domain()));
    CHECK(executor.report().status.code() == StatusCode::unsupported);
    CHECK(unavailable.writes == 0 && unavailable.reads == 0 && unavailable.flushes == 1);
}

void bounds_and_ownership()
{
    Device device;
    const auto initial = device.live;
    auto io = bind(device);
    Block block{};
    CHECK(!io.read_block(layout.journal_blocks, block));
    CHECK(!io.read_block(0, std::span(block).first(1023)));
    CHECK(!io.write_block(layout.filesystem_blocks, block));
    CHECK(!io.write_block(layout.journal_first_block, block));
    CHECK(!io.write_block(layout.journal_first_block + layout.journal_blocks - 1, block));
    CHECK(!io.write_block(~u64{0}, block));
    CHECK(device.events.empty());
    for (const auto range : std::array<BlockRange, 6>{{{128, 1}, {127, 2}, {1151, 2}, {1200, 1},
                                                       {~u64{0} - 1, 4}, {32, 0}}})
    {
        const std::array<BlockRange, 1> denied{range};
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, {layout.filesystem_blocks, denied}));
        CHECK(device.events.empty() && !executor.report().destination_may_have_changed());
    }
    ReplayExecutor mismatch;
    CHECK(!mismatch.run(io, uuid, {layout.filesystem_blocks - 1, ranges}));
    CHECK(device.events.empty());
    const std::array<BlockRange, 2> two_ranges{{{32, 32}, {64, 32}}};
    ReplayExecutor<64, 64, 1> short_domain;
    CHECK(!short_domain.run(io, uuid, {layout.filesystem_blocks, two_ranges}));
    CHECK(short_domain.report().status.code() == StatusCode::no_memory && device.events.empty());
    ReplayExecutor<1> short_actions;
    CHECK(!short_actions.run(io, uuid, domain()));
    CHECK(short_actions.report().status.code() == StatusCode::no_memory && device.writes == 0);
    CHECK(device.live == initial && device.durable == initial);
    auto wrong_uuid = uuid;
    wrong_uuid[0] ^= 1;
    ReplayExecutor wrong_identity;
    CHECK(!wrong_identity.run(io, wrong_uuid, domain()) && device.writes == 0);

    for (const auto invalid : std::array<RecoveryLayout, 6>{{
             {~u64{0}, 1200, 128, 1024, 1024}, {7, ~u64{0}, 128, 1024, 1024},
             {7, 1200, 1199, 1024, 1024}, {7, 1200, 128, 1023, 1024},
             {7, 1200, 128, 1024, 512}, {7, 0, 128, 1024, 1024}}})
        CHECK(!RecoveryIo::open(device, invalid));
    for (const auto size : {0U, 513U, 2048U})
    {
        device.reported.block_size = size;
        CHECK(!RecoveryIo::open(device, layout));
    }
    device.reported.block_size = 512;
    device.reported.writable = false;
    CHECK(!RecoveryIo::open(device, layout));
}

void geometry_changes()
{
    Device final_flush;
    const auto expected = golden_oracle(final_flush);
    auto final_io = bind(final_flush);
    final_flush.change_geometry_after_flush = 2;
    ReplayExecutor final_executor;
    CHECK(!final_executor.run(final_io, uuid, domain()));
    CHECK(final_executor.report().failed_phase == ReplayPhase::flushing);
    CHECK(final_executor.report().status.code() == StatusCode::io_error);
    CHECK(final_executor.report().flush_attempts == 2 && final_executor.report().completed_flushes == 1);
    CHECK(!final_executor.report().replay_durable());
    CHECK(final_flush.writes == 2 && final_flush.flushes == 2);
    CHECK(final_flush.durable == expected);

    for (const auto after_read : {1U, 10U, 12U, 13U, 14U})
    {
        Device device;
        const auto initial = device.live;
        auto io = bind(device);
        device.change_geometry_after_read = after_read;
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain()));
        CHECK(!executor.report().replay_durable());
        // The completed read itself must detect a replaced or resized device.
        CHECK(device.reads == after_read && device.writes == (after_read == 14 ? 1U : 0U));
        journal_unchanged(device, initial);
    }
    Device device;
    const auto initial = device.live;
    auto io = bind(device);
    device.change_geometry_after_write = 1;
    ReplayExecutor executor;
    CHECK(!executor.run(io, uuid, domain()));
    CHECK(device.writes == 1 && device.flushes == 1 && executor.report().completed_writes == 0);
    CHECK(!executor.report().replay_durable());
    journal_unchanged(device, initial);
}

void replay_order_and_tail()
{
    auto fixture = golden;
    put(fixture[1], 12, 41); // Both first-transaction tags write target 41.
    seal(fixture[1], 1020);
    put(fixture[6], 12, 41); // The next transaction reuses the same target.
    seal(fixture[6], 1020);
    Device repeated(fixture);
    auto expected = repeated.live;
    store(expected, sector(layout, 41), std::as_bytes(std::span(golden[7])));
    auto io = bind(repeated);
    ReplayExecutor executor;
    CHECK(executor.run(io, uuid, domain()));
    const auto writes = write_events(repeated);
    CHECK(writes.size() == 3 && repeated.live == expected && repeated.durable == expected);
    const std::array<Block, 3> ordered{golden[2], restored(golden[3]), golden[7]};
    for (usize index = 0; index < writes.size(); ++index)
    {
        CHECK(writes[index].sector == sector(layout, 41));
        CHECK(std::equal(writes[index].bytes.begin(), writes[index].bytes.end(),
                         std::as_bytes(std::span(ordered[index])).begin()));
    }

    fixture = golden;
    fixture[8].fill(0); // Uncommitted second-transaction data and revoke are discarded.
    Device incomplete(fixture);
    expected = incomplete.live;
    store(expected, sector(layout, 40), std::as_bytes(std::span(golden[2])));
    const auto escaped = restored(golden[3]);
    store(expected, sector(layout, 41), std::as_bytes(std::span(escaped)));
    auto incomplete_io = bind(incomplete);
    ReplayExecutor incomplete_run;
    CHECK(incomplete_run.run(incomplete_io, uuid, domain()));
    CHECK(incomplete_run.report().incomplete_tail && incomplete_run.report().committed_transactions == 1);
    CHECK(incomplete_run.report().revoked_actions == 0 && incomplete_run.report().next_sequence == 0);
    CHECK(incomplete.durable == expected);

    fixture = golden;
    put(fixture[5], 12, 24);
    put(fixture[5], 20, 41);
    seal(fixture[5], 1020);
    fixture[6] = golden[8];
    fixture[7] = golden[9];
    Device revoked(fixture);
    expected = revoked.live;
    auto revoked_io = bind(revoked);
    ReplayExecutor revoked_run;
    CHECK(revoked_run.run(revoked_io, uuid, domain()));
    CHECK(revoked_run.report().planned_actions == 0 && revoked_run.report().revoked_actions == 2);
    CHECK(revoked_run.report().committed_transactions == 2 && revoked_run.report().replay_durable());
    CHECK(revoked.writes == 0 && revoked.flushes == 2 && revoked.durable == expected);
    ReplayExecutor<64, 1> short_revokes;
    CHECK(!short_revokes.run(revoked_io, uuid, domain()));
    CHECK(short_revokes.report().status.code() == StatusCode::no_memory && revoked.writes == 0);

    fixture = golden;
    put(fixture[0], 28, 0);
    seal(fixture[0], 252, true);
    Device empty(fixture);
    expected = empty.live;
    auto empty_io = bind(empty);
    ReplayExecutor empty_run;
    CHECK(empty_run.run(empty_io, uuid, domain()));
    CHECK(empty_run.report().empty_log && empty_run.report().committed_transactions == 0);
    CHECK(empty_run.report().replay_durable() && !empty_run.report().destination_may_have_changed());
    CHECK(empty.reads == 1 && empty.writes == 0 && empty.flushes == 2 && empty.durable == expected);
}

void high_targets_and_transfer_sizes()
{
    constexpr u64 high = u64{1} << 32;
    auto fixture = golden;
    put(fixture[0], 40, 19);
    seal(fixture[0], 252, true);
    put(fixture[1], 20, 1);
    put(fixture[1], 52, 1);
    seal(fixture[1], 1020);
    put(fixture[6], 20, 1);
    seal(fixture[6], 1020);
    put(fixture[5], 12, 24);
    put(fixture[5], 16, 1);
    put(fixture[5], 20, 40);
    seal(fixture[5], 1020);
    auto large = layout;
    large.filesystem_blocks += high;
    Device device(fixture, large);
    const auto initial = device.live;
    auto expected = initial;
    const auto escaped = restored(golden[3]);
    store(expected, sector(large, high + 41), std::as_bytes(std::span(escaped)));
    store(expected, sector(large, high + 42), std::as_bytes(std::span(golden[7])));
    auto io = bind(device, large);
    const std::array<BlockRange, 1> high_range{{{high + 32, 64}}};
    ReplayExecutor executor;
    CHECK(executor.run(io, uuid, {large.filesystem_blocks, high_range}));
    CHECK(device.live == expected && device.durable == expected);
    journal_unchanged(device, initial, large);

    for (const auto block_size : {2048U, 4096U})
    {
        // Extend and independently reseal every record and payload, so this
        // exercises full transfers and escape restoration beyond a 1 KiB prefix.
        std::array<std::array<u8, 4096>, 10> grown{};
        for (usize index = 0; index < grown.size(); ++index)
            std::copy(golden[index].begin(), golden[index].end(), grown[index].begin());
        put(grown[0], 12, block_size);
        seal(std::span(grown[0]).first(1024), 252, true);
        for (const auto index : {2U, 3U, 7U})
            for (usize offset = 1024; offset < block_size; ++offset)
                grown[index][offset] = static_cast<u8>(offset * 31 + index);
        payload_checksum(grown[1], 12, 0xffffffffU, std::span(grown[2]).first(block_size));
        payload_checksum(grown[1], 44, 0xffffffffU, std::span(grown[3]).first(block_size));
        payload_checksum(grown[6], 12, 0, std::span(grown[7]).first(block_size));
        for (const auto index : {1U, 5U, 6U})
        {
            put(grown[index], 1020, 0);
            seal(std::span(grown[index]).first(block_size), block_size - 4);
        }
        for (const auto index : {4U, 8U, 9U})
            seal(std::span(grown[index]).first(block_size), 16);
        auto mapping = layout;
        mapping.block_size = block_size;
        Device full(golden, mapping);
        for (usize index = 0; index < grown.size(); ++index)
        {
            const u64 journal_block = index == 0 ? 0 : 1008 + (14 + index - 1) % 16;
            store(full.live, sector(mapping, mapping.journal_first_block + journal_block),
                  std::as_bytes(std::span(grown[index]).first(block_size)));
        }
        full.durable = full.live;
        const auto full_initial = full.live;
        auto full_expected = full_initial;
        auto full_escaped = grown[3];
        put(full_escaped, 0, magic);
        store(full_expected, sector(mapping, 41), std::as_bytes(std::span(full_escaped).first(block_size)));
        store(full_expected, sector(mapping, 42), std::as_bytes(std::span(grown[7]).first(block_size)));
        auto full_io = bind(full, mapping);
        ReplayExecutor full_run;
        CHECK(full_run.run(full_io, uuid, {mapping.filesystem_blocks, ranges}));
        CHECK(full_run.report().planned_actions == 2 && full_run.report().revoked_actions == 1);
        CHECK(full_run.report().replay_durable() && full.writes == 2 && full.flushes == 2);
        CHECK(full.live == full_expected && full.durable == full_expected);
        for (const auto &write : write_events(full)) CHECK(write.bytes.size() == block_size);
        journal_unchanged(full, full_initial, mapping);

        fixture = golden;
        put(fixture[0], 12, block_size);
        put(fixture[0], 28, 0);
        seal(fixture[0], 252, true);
        Device larger(fixture, mapping);
        auto larger_io = bind(larger, mapping);
        ReplayExecutor larger_run;
        CHECK(larger_run.run(larger_io, uuid, {mapping.filesystem_blocks, ranges}));
        CHECK(larger_run.report().empty_log && larger_run.report().replay_durable() && larger.reads == 1);
    }
    fixture = golden;
    put(fixture[0], 12, 2048);
    seal(fixture[0], 252, true);
    Device mismatch(fixture);
    auto mismatch_io = bind(mismatch);
    ReplayExecutor mismatch_run;
    CHECK(!mismatch_run.run(mismatch_io, uuid, domain()));
    CHECK(mismatch.writes == 0 && mismatch_run.report().status.code() == StatusCode::invalid_argument);
}

void descriptor_uuid_policy()
{
    // Independent libext2fs seals keep the nonzero journal/checksum UUID while
    // modelling Linux internal descriptor tags as a separate zero byte form.
    auto internal_fixture = golden;
    for (const usize index : {usize{1}, usize{6}})
    {
        std::fill_n(internal_fixture[index].begin() + 28, 16, u8{0});
        seal(internal_fixture[index], 1020);
    }
    const auto strict = Codec::open(golden[0], layout.journal_blocks, uuid);
    const auto internal = Codec::open(golden[0], layout.journal_blocks, uuid,
                                      DescriptorUuidPolicy::linux_internal_zero);
    CHECK(strict && internal);
    usize visits = 0;
    auto visit = [&](const Tag &) { ++visits; return Status::success(); };
    CHECK(strict.value().descriptor(golden[1], 0xffffffffU, visit) && visits == 2);
    visits = 0;
    CHECK(!internal.value().descriptor(golden[1], 0xffffffffU, visit) && visits == 0);
    CHECK(!strict.value().descriptor(internal_fixture[1], 0xffffffffU, visit) && visits == 0);
    CHECK(internal.value().descriptor(internal_fixture[1], 0xffffffffU, visit) && visits == 2);
    auto wrong_uuid = uuid;
    wrong_uuid[0] ^= 1;
    CHECK(!Codec::open(golden[0], layout.journal_blocks, wrong_uuid,
                       DescriptorUuidPolicy::linux_internal_zero));
    CHECK(!Codec::open(golden[0], layout.journal_blocks, uuid, static_cast<DescriptorUuidPolicy>(255)));
    auto wrong_seed = internal_fixture[1];
    const std::array<u8, 16> zero_uuid{};
    put(wrong_seed, 1020, 0);
    const u32 seed = ext2fs_crc32c_le(~u32{0}, zero_uuid.data(), zero_uuid.size());
    put(wrong_seed, 1020, ext2fs_crc32c_le(seed, wrong_seed.data(), wrong_seed.size()));
    visits = 0;
    CHECK(!internal.value().descriptor(wrong_seed, 0xffffffffU, visit) && visits == 0);
    {
        Device device(internal_fixture);
        const Disk before = device.live;
        auto io = bind(device);
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain()));
        CHECK(executor.report().failed_phase == ReplayPhase::planning && device.writes == 0);
        CHECK(device.live == before && device.durable == before);
    }
    {
        Device device(internal_fixture);
        const Disk before = device.live;
        const Disk expected = golden_oracle(device);
        auto io = bind(device);
        ReplayExecutor executor;
        CHECK(executor.run(io, uuid, domain(), DescriptorUuidPolicy::linux_internal_zero));
        CHECK(executor.report().replay_durable() && device.writes == 2 && device.flushes == 2);
        CHECK(device.live == expected && device.durable == expected);
        journal_unchanged(device, before);
    }
    // Reject mixed nonzero/zero and foreign explicit tags even in later
    // transactions, before the executor publishes any home-block write.
    for (const usize index : {usize{1}, usize{6}})
    {
        for (const bool foreign : {false, true})
        {
            auto bad = internal_fixture;
            bad[index] = golden[index];
            if (foreign) bad[index][28] ^= 1;
            seal(bad[index], 1020);
            Device device(bad);
            const Disk before = device.live;
            auto io = bind(device);
            ReplayExecutor executor;
            CHECK(!executor.run(io, uuid, domain(), DescriptorUuidPolicy::linux_internal_zero));
            CHECK(device.writes == 0 && device.live == before && device.durable == before);
        }
    }
    {
        Device device(internal_fixture);
        auto io = bind(device);
        ReplayExecutor executor;
        CHECK(!executor.run(io, uuid, domain(), static_cast<DescriptorUuidPolicy>(255)));
        CHECK(device.writes == 0 && !executor.report().replay_durable());
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    auto *input = std::fopen(argv[1], "rb");
    CHECK(input != nullptr);
    CHECK(std::fread(golden.data(), sizeof(Block), golden.size(), input) == golden.size());
    CHECK(std::fgetc(input) == EOF);
    CHECK(std::fclose(input) == 0);
    descriptor_uuid_policy();
    success_and_repetition();
    read_failures();
    write_and_flush_failures();
    bounds_and_ownership();
    geometry_changes();
    replay_order_and_tail();
    high_targets_and_transfer_sizes();
    std::puts("PASS JBD2 executable replay host model: exact device bytes, I/O failures, durable flush and restart");
    std::puts("No native device execution, ext4 mount admission, journal checkpoint or crash interoperability claim");
}
