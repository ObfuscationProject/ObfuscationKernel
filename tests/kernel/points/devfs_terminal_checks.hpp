#pragma once

#include "ok/fs/devfs.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <string_view>

namespace ok::tests
{
// Device filesystem names and namespace view only (docs/modules/tty.md
// section 2): a local filesystem and namespace, no VFS mount, open path or
// guest claim.
inline Status check_devfs_terminal_namespace_model()
{
    fs::DeviceFileSystem devfs;
    fs::TerminalNamespace terminals;
    constexpr std::array<std::string_view, 7> names{"null", "zero", "full", "tty", "console", "ptmx", "pts"};
    constexpr std::array<fs::FilesystemDevice, 6> numbers{{{1, 3}, {1, 5}, {1, 7}, {5, 0}, {5, 1}, {5, 2}}};
    auto listing = devfs.root().list();
    if (!listing || listing.value().count != names.size()) return Status::fault("devfs root does not list seven nodes");
    for (usize i = 0; i < names.size(); ++i)
    {
        const auto &entry = listing.value().entries[i];
        if (entry.name.view() != names[i]) return Status::fault("devfs root node names differ");
        if (i < numbers.size() && (entry.metadata.attributes.special != numbers[i] ||
                                   !entry.metadata.attributes.has(fs::Attribute::special_device)))
            return Status::fault("devfs device numbers differ");
    }
    auto *pts = devfs.root().lookup("pts");
    if (!pts || !pts->list() || pts->list().value().count) return Status::fault("unattached /dev/pts is not empty");
    if (auto status = devfs.attach_terminals(terminals); !status) return status;
    terminals.publish(3, {.generation = 9, .uid = 1000, .gid = 5, .present = true, .locked = true});
    auto *entry = pts->lookup("3");
    if (!entry || pts->list().value().count != 1 || entry->metadata().attributes.special != fs::FilesystemDevice{136, 3} ||
        entry->terminal_node() != fs::TerminalNode{fs::TerminalNodeKind::pts_slave, 3, 9})
        return Status::fault("locked /dev/pts entry is not listed");
    for (const auto name : {"03", "8", "-3", "", "3x"})
        if (pts->lookup(name)) return Status::fault("/dev/pts lookup is not strict decimal");
    terminals.remove(3, 9);
    if (pts->lookup("3") || entry->query_metadata().status().code() != StatusCode::not_found)
        return Status::fault("removed /dev/pts entry is not stale");
    return {};
}

// Scheduler terminal opens only (docs/modules/tty.md sections 3 and 4): one
// ptmx pair, its locked and unlocked slave, the automatic acquisition and
// /dev/tty, on a temporary session leader. No path walk, trap or guest claim.
inline Status check_tty_open_model(sched::SchedulerState &scheduler)
{
    using Kind = fs::TerminalNodeKind;
    const auto descriptions_before = scheduler.open_description_count();
    const auto pairs_before = scheduler.pty_audit().live_pairs;
    auto made = scheduler.create_process("tty-open-check-model", {});
    if (!made) return made.status();
    const auto pid = made.value();
    std::array<i32, 4> fds{-1, -1, -1, -1};
    const auto open = [&](usize slot, fs::TerminalNode node, bool noctty) -> Result<sched::TerminalOpenResult> {
        auto *process = scheduler.find(pid);
        auto id = scheduler.allocate_description({});
        if (!id) return id.status();
        if (auto status = scheduler.retain_description(id.value()); !status) return status;
        i32 fd = 3;
        while (process->resources().files.contains(fd)) ++fd;
        auto bound = scheduler.open_terminal(pid, id.value(), {.node = node, .flags = 2u, .readable = true,
                                                                .writable = true, .no_controlling_terminal = noctty});
        Status status = bound.status();
        if (status) status = scheduler.install_descriptor(process->resources(), fd, id.value());
        if (status) fds[slot] = fd;
        if (auto released = scheduler.release_description(id.value()); status && !released) status = released;
        if (!status) return status;
        return bound;
    };
    const auto description = [&](usize slot) {
        return scheduler.find(pid)->resources().files.entry(fds[slot])->description;
    };
    const auto result = [&]() -> Status {
        if (scheduler.open_terminal(pid, 0xffff, {.node = {Kind::console}, .readable = true}))
            return Status::fault("terminal open accepted a missing description");
        auto tty = open(3, {Kind::controlling}, false);
        if (tty || tty.status().code() != StatusCode::no_device) return Status::fault("/dev/tty without a ctty is not ENXIO");
        auto master = open(0, {Kind::multiplexer}, false);
        if (!master) return master.status();
        const auto index = master.value().index;
        const auto entry = scheduler.terminal_namespace().entry(index);
        if (!entry.present || !entry.locked) return Status::fault("new ptmx pair is not listed locked");
        const fs::TerminalNode slave{Kind::pts_slave, index, entry.generation};
        auto locked = open(1, slave, false);
        if (locked || locked.status().code() != StatusCode::io_error) return Status::fault("locked slave open is not EIO");
        if (auto status = scheduler.set_pty_lock(description(0), false); !status) return status;
        auto quiet = open(1, slave, true);
        if (!quiet || quiet.value().acquired) return Status::fault("O_NOCTTY slave open acquired");
        auto taken = open(2, slave, false);
        if (!taken || !taken.value().acquired) return Status::fault("session leader slave open did not acquire");
        tty = open(3, {Kind::controlling}, false);
        if (!tty || scheduler.open_description(description(3))->pty_endpoint !=
                        scheduler.open_description(description(1))->pty_endpoint)
            return Status::fault("/dev/tty did not open the controlling terminal");
        const auto status = scheduler.terminal_status(description(1));
        if (!status || status.value().rdev != fs::FilesystemDevice{136, index} || status.value().mode != 0620)
            return Status::fault("pts description status differs");
        return scheduler.terminal_audit();
    }();
    auto *process = scheduler.find(pid);
    Status cleanup{};
    for (const auto fd : fds)
        if (fd >= 0 && process)
            if (auto closed = scheduler.close_descriptor(process->resources(), fd); cleanup && !closed) cleanup = closed;
    if (auto killed = scheduler.kill_process(pid); cleanup && !killed) cleanup = killed;
    if (!result) return result;
    if (!cleanup) return cleanup;
    if (scheduler.open_description_count() != descriptions_before || scheduler.pty_audit().live_pairs != pairs_before)
        return Status::fault("terminal open check leaked a description or pair");
    return {};
}
} // namespace ok::tests
