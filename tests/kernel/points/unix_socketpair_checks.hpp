#pragma once
#include "ok/sched/scheduler.hpp"

namespace ok::tests
{
// Canonical OFD/buffer ownership model. This executes no user syscall.
inline Status check_unix_socketpair_ownership(sched::SchedulerState &scheduler)
{
    auto &resources = scheduler.boot_resources();
    const auto before = scheduler.open_description_count();
    auto pair = scheduler.create_unix_socketpair(resources, true, 0x0800u);
    if (!pair) return pair.status();
    struct Cleanup
    {
        sched::SchedulerState &scheduler;
        sched::ProcessResources &resources;
        std::array<i32, 2> fds;
        u16 pinned{sched::invalid_description};
        ~Cleanup()
        {
            for (auto fd : fds) static_cast<void>(scheduler.close_descriptor(resources, fd));
            if (pinned != sched::invalid_description) static_cast<void>(scheduler.release_description(pinned));
        }
    } cleanup{scheduler, resources, pair.value()};
    const auto left_id = resources.files.entry(pair.value()[0])->description;
    const auto right_id = resources.files.entry(pair.value()[1])->description;
    const auto *left = scheduler.open_description(left_id);
    const auto *right = scheduler.open_description(right_id);
    if (!left || !right || left_id == right_id || left->pipe == right->pipe ||
        left->pipe != right->unix_write_pipe || right->pipe != left->unix_write_pipe ||
        !resources.files.entry(pair.value()[0])->close_on_exec || left->flags != (2u | 0x0800u))
        return Status::fault("socketpair directional ownership mismatch");
    const auto inbound = right->pipe;
    std::array<std::byte, 2> input{std::byte{'a'}, std::byte{'b'}}, output{};
    auto write = scheduler.write_pipe(left->unix_write_pipe, input, true, false);
    if (!write || write.value() != 2) return Status::fault("socketpair model write failed");
    if (auto status = scheduler.retain_description(left_id); !status) return status;
    cleanup.pinned = left_id;
    if (auto status = scheduler.close_descriptor(resources, pair.value()[0]); !status) return status;
    auto held = scheduler.pipe_readiness(inbound);
    if (!held || !held.value().writers) return Status::fault("socketpair pin lost the peer endpoint");
    if (auto status = scheduler.release_description(left_id); !status) return status;
    cleanup.pinned = sched::invalid_description;
    auto ready = scheduler.pipe_readiness(inbound);
    if (!ready || ready.value().writers || ready.value().buffered != 2)
        return Status::fault("socketpair final release lost buffered EOF ownership");
    auto read = scheduler.read_pipe(inbound, output, true);
    if (!read || read.value() != 2 || output != input)
        return Status::fault("socketpair buffered bytes were lost before EOF");
    read = scheduler.read_pipe(inbound, output, true);
    if (!read || read.value()) return Status::fault("socketpair model did not reach EOF");
    if (auto status = scheduler.close_descriptor(resources, pair.value()[1]); !status) return status;
    if (scheduler.open_description_count() != before)
        return Status::fault("socketpair final close leaked an open description");
    // More iterations than the buffer capacity expose leaked reservations.
    for (usize iteration = 0; iteration <= sched::max_process_pipes; ++iteration)
    {
        auto fresh = scheduler.create_unix_socketpair(resources);
        if (!fresh) return fresh.status();
        for (auto fd : fresh.value())
            if (auto status = scheduler.close_descriptor(resources, fd); !status) return status;
    }
    return scheduler.open_description_count() == before ? Status::success()
        : Status::fault("socketpair reservation reuse leaked descriptions");
}
} // namespace ok::tests
