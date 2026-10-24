#pragma once
#include "ok/user/native_pipe_vectors.hpp"
#include "ok/sched/scheduler.hpp"
namespace ok::tests
{
// Metadata model only; native guest tests exercise copying, traps and I/O.
inline Status check_pipe_vector_metadata()
{
    user::native::PipeVectors request{};
    request.count = 4;
    request.entries[0] = {0x1000, 2048}; request.entries[1] = {~uptr{0}, 0};
    request.entries[2] = {0x3000, 2048}; request.entries[3] = {0x5000, 1};
    if (request.staged_count(4096) != 4096 || request.staged_count(4097) != 4097)
        return Status::fault("pipe vector staging lost oversized atomicity boundary");
    const auto original = request;
    ++request.entries[1].base;
    if (request == original) return Status::fault("pipe vector identity lost zero-length descriptor");
    request = original; request.entries[0].length = static_cast<usize>(__INTPTR_MAX__);
    if (request.staged_count(4097)) return Status::fault("pipe vector total overflow accepted");
    request = original; request.count = user::native::PipeVectors::capacity + 1;
    if (request.staged_count(4097)) return Status::fault("pipe vector descriptor overflow accepted");
    return Status::success();
}
// Scheduler model only. This does not execute a user syscall or wait continuation.
inline Status check_pipe_write_fragment(sched::SchedulerState &scheduler)
{
    auto &resources = scheduler.boot_resources();
    auto fds = scheduler.create_pipe(resources);
    if (!fds) return fds.status();
    struct Close
    {
        sched::SchedulerState &scheduler;
        sched::ProcessResources &resources;
        std::array<i32, 2> fds;
        ~Close() { for (auto fd : fds) static_cast<void>(scheduler.close_descriptor(resources, fd)); }
    } close{scheduler, resources, fds.value()};
    const auto *descriptor = resources.files.entry(fds.value()[1]);
    const auto *description = descriptor ? scheduler.open_description(descriptor->description) : nullptr;
    if (!description) return Status::fault("pipe fragment model lost description");
    const auto pipe = description->pipe;
    std::array<std::byte, sched::pipe_capacity> bytes{};
    auto fill = scheduler.write_pipe(pipe, {bytes.data(), bytes.size() - 1}, true);
    if (!fill || fill.value() != bytes.size() - 1) return Status::fault("pipe fragment model fill failed");
    auto atomic = scheduler.write_pipe(pipe, {bytes.data(), 2}, true);
    if (atomic || atomic.status().code() != StatusCode::would_block)
        return Status::fault("existing pipe atomicity changed");
    auto fragment = scheduler.write_pipe_fragment(pipe, {bytes.data(), 2}, false);
    if (!fragment || fragment.value() != 1) return Status::fault("large suffix lost partial publication");
    auto ready = scheduler.pipe_readiness(pipe);
    if (!ready || ready.value().available) return Status::fault("partial fragment did not fill pipe");
    auto read = scheduler.read_pipe(pipe, bytes, true);
    if (!read || read.value() != bytes.size()) return Status::fault("pipe fragment drain failed");
    fragment = scheduler.write_pipe_fragment(pipe, {bytes.data(), 1}, false);
    if (!fragment || fragment.value() != 1) return Status::fault("pipe fragment final suffix failed");
    return Status::success();
}
} // namespace ok::tests
