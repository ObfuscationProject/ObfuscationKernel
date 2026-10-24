#pragma once

#include "ok/sched/scheduler.hpp"

namespace ok::tests
{
// Actual scheduler/OFD ownership model only. No trap, user copy or guest claim.
inline Status check_native_pty_ownership_model(sched::SchedulerState &scheduler)
{
    const auto descriptions_before = scheduler.open_description_count();
    const auto processes_before = scheduler.process_count();
    const auto audit_before = scheduler.pty_audit();
    auto process = scheduler.create_process("native-pty-check-model", {});
    if (!process) return process.status();
    const auto pid = process.value();
    struct Cleanup
    {
        sched::SchedulerState &owner;
        sched::ProcessId pid;
        std::array<i32, 2> descriptors{-1, -1};
        u16 retained{sched::invalid_description};
        Status close(usize side)
        {
            if (descriptors[side] < 0) return {};
            const auto fd = std::exchange(descriptors[side], -1);
            auto *process = owner.find(pid);
            return process ? owner.close_descriptor(process->resources(), fd)
                           : Status::fault("PTY boot check process disappeared");
        }
        Status release()
        {
            if (retained == sched::invalid_description) return {};
            return owner.release_description(std::exchange(retained, sched::invalid_description));
        }
        Status finish()
        {
            Status first{};
            auto remember = [&](Status result) { if (first && !result) first = result; };
            remember(close(0)); remember(close(1)); remember(release());
            remember(owner.kill_process(pid));
            return first;
        }
    } cleanup{scheduler, pid};
    const auto result = [&]() -> Status
    {
        auto *owner = scheduler.find(pid);
        if (!owner) return Status::fault("PTY boot check process is missing");
        const auto baseline_fds = owner->resources().files.open_count();
        {
            auto prepared = scheduler.prepare_pty_pair(pid, 0x0800u | 0x080000u);
            if (!prepared) return prepared.status();
            if (auto valid = prepared.value().validate(); !valid) return valid;
            const auto fds = prepared.value().descriptors();
            for (const auto fd : fds)
            {
                const auto *slot = owner->resources().files.entry(fd);
                if (!slot || slot->used || !slot->reservation)
                    return Status::fault("PTY boot preparation published a descriptor early");
            }
            if (owner->resources().prepared_descriptor_operations != 2 ||
                owner->resources().files.open_count() != baseline_fds)
                return Status::fault("PTY boot preparation accounting differs");
            if (auto cancelled = prepared.value().cancel(); !cancelled) return cancelled;
        }
        if (owner->resources().prepared_descriptor_operations ||
            scheduler.open_description_count() != descriptions_before ||
            scheduler.pty_audit().live_endpoints != audit_before.live_endpoints)
            return Status::fault("PTY boot cancellation leaked ownership");

        auto prepared = scheduler.prepare_pty_pair(pid, 0x0800u | 0x080000u);
        if (!prepared) return prepared.status();
        if (auto valid = prepared.value().validate(); !valid) return valid;
        cleanup.descriptors = prepared.value().descriptors();
        prepared.value().publish_checked();
        const auto *master_slot = owner->resources().files.entry(cleanup.descriptors[0]);
        const auto *slave_slot = owner->resources().files.entry(cleanup.descriptors[1]);
        if (!master_slot || !slave_slot || !master_slot->used || !slave_slot->used ||
            !master_slot->close_on_exec || !slave_slot->close_on_exec ||
            master_slot->description == slave_slot->description)
            return Status::fault("PTY boot publication lost distinct CLOEXEC descriptions");
        const auto master = master_slot->description, slave = slave_slot->description;
        const auto *master_ofd = scheduler.open_description(master);
        const auto *slave_ofd = scheduler.open_description(slave);
        if (!master_ofd || !slave_ofd || !(master_ofd->flags & 0x0800u) ||
            !master_ofd->readable || !master_ofd->writable || !slave_ofd->readable || !slave_ofd->writable)
            return Status::fault("PTY boot canonical access/status flags differ");
        const auto original = master_ofd->pty_endpoint;
        if (!original || original.side != sched::private_pty::Side::master ||
            slave_ofd->pty_endpoint.side != sched::private_pty::Side::slave ||
            original.slot != slave_ofd->pty_endpoint.slot || original.generation != slave_ofd->pty_endpoint.generation)
            return Status::fault("PTY boot endpoint identity differs");
        const std::array<std::byte, 3> payload{std::byte{0}, std::byte{0x80}, std::byte{0xff}};
        std::array<std::byte, 3> output{};
        auto write = scheduler.write_pty_description(master, payload);
        if (!write || write.value() != payload.size()) return Status::fault("PTY boot forward write failed");
        write = scheduler.write_pty_description(slave, payload);
        if (!write || write.value() != payload.size()) return Status::fault("PTY boot reverse write failed");
        auto read = scheduler.read_pty_description(master, output);
        if (!read || read.value() != payload.size() || output != payload)
            return Status::fault("PTY boot duplex bytes differ");
        if (auto retained = scheduler.retain_description(master); !retained) return retained;
        cleanup.retained = master;
        if (auto closed = cleanup.close(0); !closed) return closed;
        auto readiness = scheduler.pty_description_readiness(slave);
        if (!readiness || readiness.value().hangup ||
            scheduler.pty_audit().live_endpoints != audit_before.live_endpoints + 2)
            return Status::fault("PTY boot nonfinal fd close retired the held OFD");
        if (auto released = cleanup.release(); !released) return released;
        readiness = scheduler.pty_description_readiness(slave);
        if (!readiness || !readiness.value().hangup || !readiness.value().error ||
            !readiness.value().readable || readiness.value().writable || readiness.value().buffered != payload.size())
            return Status::fault("PTY boot final release lost buffered terminal readiness");
        read = scheduler.read_pty_description(slave, output);
        if (!read || read.value() != payload.size() || output != payload)
            return Status::fault("PTY boot final-release drain lost accepted bytes");
        read = scheduler.read_pty_description(slave, output);
        if (!read || read.value() != 0) return Status::fault("PTY boot slave did not reach EOF");
        write = scheduler.write_pty_description(slave, payload);
        if (write || write.status().code() != StatusCode::io_error)
            return Status::fault("PTY boot retired-peer write did not return EIO");
        if (auto closed = cleanup.close(1); !closed) return closed;
        auto reused = scheduler.prepare_pty_pair(pid);
        if (!reused) return reused.status();
        if (auto valid = reused.value().validate(); !valid) return valid;
        cleanup.descriptors = reused.value().descriptors(); reused.value().publish_checked();
        const auto *new_slot = owner->resources().files.entry(cleanup.descriptors[0]);
        const auto *new_ofd = new_slot ? scheduler.open_description(new_slot->description) : nullptr;
        if (!new_ofd || new_ofd->pty_endpoint.slot != original.slot ||
            new_ofd->pty_endpoint.generation <= original.generation)
            return Status::fault("PTY boot provider slot reused an old generation");
        return {};
    }();
    // Cleanup is checked even when an earlier assertion fails; no blanket drain
    // can hide leaked references or disturb other boot-test resources.
    if (auto status = cleanup.finish(); !status) return status;
    if (!result) return result;
    const auto audit_after = scheduler.pty_audit();
    if (scheduler.open_description_count() != descriptions_before || scheduler.process_count() != processes_before ||
        audit_after.live_pairs != audit_before.live_pairs || audit_after.live_endpoints != audit_before.live_endpoints ||
        audit_after.buffered_bytes != audit_before.buffered_bytes || !audit_after.cleared_outside_live_bytes)
        return Status::fault("PTY boot checked cleanup leaked a process, OFD or provider");
    return {};
}
} // namespace ok::tests
