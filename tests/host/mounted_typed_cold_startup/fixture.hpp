#pragma once

// Hosted real-media support. Private calls are made only by source models with
// -fno-access-control; no private state, Session serial or counter is rewritten.
#include <ok/fs/ext4/mount.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <functional>

namespace typed_cold_startup_fixture
{
using namespace ok;
using namespace ok::fs;
using Block = Ext4FirstDataBlock;
using Owner = Ext4OwnedTransaction<2, 8>;
constexpr usize image_bytes = 32 * 1024 * 1024;
constexpr u32 zero_inode = 12, allocated_inode = 13, generation = 9;
constexpr u32 allocated_size = 73, allocated_offset = 7;

inline void require(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "typed cold startup fixture: %s\n", message); std::abort(); }
}
inline void require(Status status, const char *message)
{
    if (!status)
    {
        std::fprintf(stderr, "typed cold startup fixture: %s: code=%u detail=%.*s\n",
            message, unsigned(status.code()), int(status.message().size()), status.message().data());
        std::abort();
    }
}
inline std::vector<u8> load(const char *path)
{
    std::FILE *stream = std::fopen(path, "rb"); require(stream != nullptr, "open generated image");
    std::vector<u8> result(image_bytes);
    require(std::fread(result.data(), 1, result.size(), stream) == result.size(), "read generated image");
    require(std::fgetc(stream) == EOF, "generated image exact length");
    require(std::fclose(stream) == 0, "close generated image"); return result;
}
inline void save(const char *path, const std::vector<u8> &bytes)
{
    std::FILE *stream = std::fopen(path, "wbx"); require(stream != nullptr, "exclusively create fixture output");
    require(std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size(), "write fixture output");
    require(std::fclose(stream) == 0, "close fixture output");
}

struct Device final : driver::BlockDevice
{
    struct Write { u32 block{}; Block bytes{}; };
    std::vector<u8> bytes;
    std::vector<Write> writes;
    std::vector<usize> flush_boundaries;
    usize reads{}, write_attempts{}, flush_attempts{};
    usize fail_read{}, fail_write{}, fail_flush{};
    std::function<void(usize)> observe_read{};
    explicit Device(const std::vector<u8> &image) : bytes(image)
    { require(bytes.size() == image_bytes, "fixed real-media geometry"); }
    driver::BlockGeometry geometry() const override { return {image_bytes / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads;
        if (observe_read) observe_read(reads);
        if (fail_read && reads == fail_read) return Status::io_error("cold fixture selected READ failure");
        if (sector >= image_bytes / 512 || out.empty() || out.size() % 512 ||
            out.size() > image_bytes - usize(sector) * 512)
            return Status::invalid_argument("cold fixture READ bounds");
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[usize(sector) * 512 + at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        ++write_attempts;
        if (fail_write && write_attempts == fail_write) return Status::io_error("cold fixture selected WRITE failure");
        if (sector % 2 || sector >= image_bytes / 512 || in.size() != 1024 ||
            in.size() > image_bytes - usize(sector) * 512)
            return Status::invalid_argument("cold fixture WRITE bounds");
        Write record{u32(sector / 2), {}};
        for (usize at = 0; at < 1024; ++at)
            bytes[usize(sector) * 512 + at] = record.bytes[at] = std::to_integer<u8>(in[at]);
        writes.push_back(record); return {};
    }
    Status flush() override
    {
        ++flush_attempts;
        if (fail_flush && flush_attempts == fail_flush) return Status::io_error("cold fixture selected FLUSH failure");
        flush_boundaries.push_back(writes.size()); return {};
    }
    void clear_trace()
    {
        reads = write_attempts = flush_attempts = 0;
        writes.clear(); flush_boundaries.clear();
    }
};

struct NativePreparation
{
    std::vector<u8> input;
    std::vector<Device::Write> writes;
    std::vector<usize> flush_boundaries;
    Ext4RecoveryInfo binding{};
    block::SessionReport initial{}, terminal{};
    std::array<u32, 4> targets{};
    Block data{};
    u32 candidate{}, active_sequence{}, final_sequence{};
    bool allocated{};

    bool writer_crash_reachable(u8 mask) const
    {
        if (mask >= 16) return false;
        // Zero flushes the inode before allocation writes. Allocated-data
        // flushes all three allocation homes before writing its inode home.
        return allocated ? mask < 8 || mask == 15 : mask == 0 || (mask & 1) != 0;
    }

    std::vector<u8> completed() const
    {
        std::vector<u8> result(input);
        for (const auto &record : writes)
            for (usize at = 0; at < 1024; ++at)
                result[usize(record.block) * 1024 + at] = record.bytes[at];
        return result;
    }

    // A recovery image copies bytes actually written by the completed native
    // child. WAL and ordered data are durable; cleanup is excluded. Only masks
    // satisfying writer_crash_reachable represent that writer's barrier-respecting
    // crash cuts. Other masks are explicitly mixed-home recovery inputs.
    std::vector<u8> cut(u8 mask) const
    {
        require(mask < 16, "four-home cut mask");
        const usize publication_flush = allocated ? 3 : 2;
        require(flush_boundaries.size() > publication_flush, "native committed publication barrier");
        std::vector<u8> result(input);
        const auto apply = [&](const Device::Write &record) {
            require(record.block < image_bytes / 1024, "native captured write bounds");
            for (usize at = 0; at < 1024; ++at) result[usize(record.block) * 1024 + at] = record.bytes[at];
        };
        const usize committed = flush_boundaries[publication_flush];
        for (usize at = 0; at < committed; ++at) apply(writes[at]);
        for (u32 home = 0; home < 4; ++home)
            if (mask & (1U << home))
            {
                bool found{};
                for (usize at = committed; at < writes.size(); ++at)
                    if (writes[at].block == targets[home]) { apply(writes[at]); found = true; break; }
                require(found, "native captured home write exists");
            }
        return result;
    }
};

inline NativePreparation prepare(const std::vector<u8> &input, bool allocated)
{
    NativePreparation prepared; prepared.input = input; prepared.allocated = allocated;
    Device device(input);
    Ext4RecoveryBinding inspection;
    require(inspection.inspect(device, {0, device.geometry().block_count}), "actual generated binding inspection");
    require(inspection.info() != nullptr, "actual binding admitted");
    prepared.binding = *inspection.info();
    require(prepared.binding.layout.filesystem_blocks == 32768 && prepared.binding.layout.block_size == 1024 &&
        prepared.binding.metadata_count == 16 && prepared.binding.empty_log && prepared.binding.needs_recovery,
        "actual fixed original native-child input");
    Owner::Registry registry;
    auto workspace = std::unique_ptr<Owner::TypedOwnerWorkspace>(new Owner::TypedOwnerWorkspace);
    auto owner = std::unique_ptr<Owner>(new Owner(true));
    const auto key = registry.register_device(device, allocated ? 113 : 112);
    require(bool(key), "register actual generated backing");
    require(owner->open(registry, key.value(), {0, device.geometry().block_count}), "open original native Owner");
    const auto initial = owner->session_report(); require(bool(initial), "observe original native Session");
    prepared.initial = initial.value();
    require(!prepared.initial.reads && !prepared.initial.writes && !prepared.initial.flushes &&
        !prepared.initial.completed_reads && !prepared.initial.completed_writes && !prepared.initial.completed_flushes,
        "native child original Session counters untouched");
    device.clear_trace();
    if (allocated)
    {
        for (usize at = allocated_offset; at < allocated_size; ++at) prepared.data[at] = u8(1 + (at * 17) % 251);
        require(owner->run_typed_allocated(*workspace, prepared.binding,
            {allocated_inode, generation, allocated_size, 1700000222, 123456789},
            allocated_offset, prepared.data), "genuine fresh allocated child completion");
        prepared.targets = workspace->allocated_.images_.plan.targets;
        prepared.candidate = workspace->allocated_.images_.plan.physical;
        prepared.active_sequence = workspace->allocated_.images_.journal.active_sequence;
        prepared.final_sequence = workspace->allocated_.report().final_sequence;
    }
    else
    {
        require(owner->run_typed_zero(*workspace, prepared.binding,
            {zero_inode, generation, 4096, 1700000222, 123456789}), "genuine fresh zero child completion");
        prepared.targets = workspace->zero_.images_.plan.targets;
        prepared.active_sequence = workspace->zero_.images_.journal.active_sequence;
        prepared.final_sequence = workspace->zero_.report().final_sequence;
    }
    const auto terminal = owner->session_report(); require(bool(terminal), "observe actual terminal native Session");
    prepared.terminal = terminal.value();
    require(prepared.terminal.session_serial == prepared.initial.session_serial &&
        prepared.terminal.reads == prepared.terminal.completed_reads &&
        prepared.terminal.writes == prepared.terminal.completed_writes &&
        prepared.terminal.flushes == prepared.terminal.completed_flushes &&
        prepared.terminal.reads == device.reads && prepared.terminal.writes == device.write_attempts &&
        prepared.terminal.flushes == device.flush_attempts && !prepared.terminal.in_flight &&
        !prepared.terminal.pending_writes, "genuine native child accounting exact");
    require(prepared.active_sequence == prepared.binding.journal_sequence + 1 &&
        prepared.final_sequence == prepared.binding.journal_sequence + 2,
        "native formatter actual sequence history");
    prepared.writes = device.writes; prepared.flush_boundaries = device.flush_boundaries;
    require(owner->close(), "close completed original native Owner");
    require(registry.detach(key.value()), "detach completed generated backing");
    return prepared;
}

inline block::SessionReport verify_native_recovery(const NativePreparation &prepared, u8 mask)
{
    Device device(prepared.cut(mask));
    auto binding = prepared.binding;
    const auto first = binding.layout.journal_first_block;
    const auto head = std::span<const u8>{device.bytes}.subspan(usize(first) * 1024, 1024);
    const auto codec = jbd2::Codec::open(head, u32(binding.layout.journal_blocks), binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero);
    require(bool(codec), "actual committed cut journal codec");
    require(codec.value().superblock().start_block == 1 &&
        codec.value().superblock().sequence == prepared.active_sequence, "actual committed cut selection");
    binding.empty_log = false; binding.journal_sequence = codec.value().superblock().sequence;
    Owner::Registry registry;
    auto workspace = std::unique_ptr<Owner::TypedOwnerWorkspace>(new Owner::TypedOwnerWorkspace);
    auto owner = std::unique_ptr<Owner>(new Owner(true));
    const auto key = registry.register_device(device, prepared.allocated ? 213 : 212);
    require(bool(key), "register real committed cut");
    require(owner->open(registry, key.value(), {0, device.geometry().block_count}), "open genuine cut recovery Owner");
    const auto initial = owner->session_report(); require(bool(initial), "observe original cut Session");
    require(!initial.value().reads && !initial.value().writes && !initial.value().flushes,
        "cut original Session has authentic zero transport history");
    if (prepared.allocated)
        require(owner->run_typed_allocated(*workspace, binding,
            {allocated_inode, generation, allocated_size, 1700000222, 123456789},
            allocated_offset, prepared.data, true), "genuine allocated cut recovery child");
    else
        require(owner->run_typed_zero(*workspace, binding,
            {zero_inode, generation, 4096, 1700000222, 123456789}, true), "genuine zero cut recovery child");
    const auto terminal = owner->session_report(); require(bool(terminal), "observe recovered cut Session");
    require(terminal.value().session_serial == initial.value().session_serial &&
        terminal.value().reads == terminal.value().completed_reads &&
        terminal.value().writes == terminal.value().completed_writes &&
        terminal.value().flushes == terminal.value().completed_flushes &&
        terminal.value().reads == device.reads && terminal.value().writes == device.write_attempts &&
        terminal.value().flushes == device.flush_attempts && !terminal.value().in_flight &&
        !terminal.value().pending_writes, "native cut recovery accounting exact");
    require(device.bytes == prepared.completed(), "all actual recovered physical bytes match completed native child");
    require(owner->close(), "close actual recovered cut Owner");
    require(registry.detach(key.value()), "detach actual recovered cut backing");
    return terminal.value();
}
} // namespace typed_cold_startup_fixture
