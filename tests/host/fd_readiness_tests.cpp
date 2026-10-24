#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition);                                               \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
using Events = posix::ReadyEvents;
constexpr Events io_events = Events::readable | Events::writable;
std::span<const std::byte> bytes(std::string_view value)
{
    return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}
struct Fixture
{
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    sched::ProcessId pid;
    Fixture()
    {
        CHECK(console.start());
        CHECK(service.initialize(vfs, console, scheduler));
        CHECK(vfs.create("/file", fs::NodeType::regular));
        auto process = scheduler.create_user_process(
            "readiness-model", arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(process);
        pid = process.value();
        CHECK(scheduler.set_runnable(pid) && service.activate(pid));
    }
    void running()
    {
        const auto *process = scheduler.find(pid);
        CHECK(process && process->state() == sched::ProcessState::running);
        CHECK(process->threads()[0].state == sched::ProcessState::running);
        CHECK(process->threads()[0].wait_reason == sched::ThreadWaitReason::none);
    }
    void ready(posix::Fd fd, Events expected, Events requested = io_events)
    {
        auto result = service.ready_events(fd, requested);
        CHECK(result && result.value().events == expected && !result.value().console_input);
    }
    u16 description(posix::Fd fd)
    {
        auto *slot = scheduler.find(service.getpid())->fd_table().entry(fd);
        CHECK(slot && slot->used);
        return slot->description;
    }
};
void nonblocking_and_atomic_writes()
{
    static Fixture f;
    CHECK(f.service.fcntl(0, posix::f_GETFL, 0).value() == posix::o_RDONLY);
    CHECK(f.service.fcntl(1, posix::f_GETFL, 0).value() == posix::o_WRONLY);
    CHECK(f.service.fcntl(2, posix::f_GETFL, 0).value() == posix::o_WRONLY);
    const auto descriptors = f.service.open_file_count(), descriptions = f.scheduler.open_description_count();
    CHECK(f.service.pipe(posix::o_APPEND).status().code() == StatusCode::invalid_argument);
    CHECK(f.service.open_file_count() == descriptors && f.scheduler.open_description_count() == descriptions);
    auto pair = f.service.pipe(posix::o_NONBLOCK | posix::o_CLOEXEC);
    CHECK(pair);
    const auto reader = pair.value()[0], writer = pair.value()[1];
    CHECK(f.service.fcntl(reader, posix::f_GETFL, 0).value() == posix::o_NONBLOCK);
    CHECK(f.service.fcntl(writer, posix::f_GETFL, 0).value() == (posix::o_WRONLY | posix::o_NONBLOCK));
    CHECK(f.service.fcntl(reader, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    CHECK(f.service.fcntl(writer, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    f.ready(reader, Events::none);
    f.ready(writer, Events::writable);
    std::array<std::byte, sched::pipe_capacity + 1> data{};
    for (auto &byte : data)
        byte = std::byte{'A'};
    CHECK(f.service.read(reader, data).status().code() == StatusCode::would_block);
    f.running();
    CHECK(f.service.read(reader, {}).value() == 0 && f.service.write(writer, {}).value() == 0);
    CHECK(f.service.write(writer, {data.data(), sched::pipe_capacity - 2}).value() == sched::pipe_capacity - 2);
    f.ready(reader, Events::readable);
    f.ready(writer, Events::writable);
    CHECK(f.service.write(writer, bytes("XYZ")).status().code() == StatusCode::would_block);
    f.running();
    const posix::IoVector vector[] = {{reinterpret_cast<uptr>("X"), 1}, {reinterpret_cast<uptr>("YZ"), 2}};
    CHECK(f.service.writev(writer, vector).status().code() == StatusCode::would_block);
    f.running();
    auto state = f.scheduler.pipe_readiness(f.scheduler.open_description(f.description(reader))->pipe);
    CHECK(state && state.value().buffered == sched::pipe_capacity - 2);
    CHECK(f.service.write(writer, bytes("BC")).value() == 2);
    f.ready(writer, Events::none);
    CHECK(f.service.write(writer, bytes("D")).status().code() == StatusCode::would_block);
    f.running();
    CHECK(f.service.read(reader, data).value() == sched::pipe_capacity);
    for (usize i = 0; i < sched::pipe_capacity - 2; ++i)
        CHECK(data[i] == std::byte{'A'});
    CHECK(data[sched::pipe_capacity - 2] == std::byte{'B'} && data[sched::pipe_capacity - 1] == std::byte{'C'});
    CHECK(f.service.writev(writer, vector).value() == 3);
    CHECK(f.service.read(reader, data).value() == 3 && std::memcmp(data.data(), "XYZ", 3) == 0);
    CHECK(f.service.write(writer, data).value() == sched::pipe_capacity); // Large writes may be partial.
    CHECK(f.service.read(reader, {data.data(), 2}).value() == 2);
    CHECK(f.service.write(writer, data).value() == 2);
    f.running();
    CHECK(f.service.close(reader));
    f.ready(writer, Events::error, Events::none);
    CHECK(f.service.write(writer, bytes("x")).status().code() == StatusCode::broken_pipe);
    f.running();
    CHECK(f.service.close(writer));
}
void shared_status_and_peer_close()
{
    static Fixture f;
    auto pair = f.service.pipe(posix::o_NONBLOCK | posix::o_CLOEXEC);
    CHECK(pair);
    const auto reader = pair.value()[0], writer = pair.value()[1];
    auto alias = f.service.duplicate(reader);
    CHECK(alias);
    CHECK(f.service.fcntl(alias.value(), posix::f_GETFD, 0).value() == 0);
    CHECK(f.service.fcntl(alias.value(), posix::f_SETFL, posix::o_WRONLY | posix::o_APPEND | posix::o_CLOEXEC));
    CHECK(f.service.fcntl(reader, posix::f_GETFL, 0).value() == posix::o_APPEND);
    CHECK(f.service.fcntl(reader, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    auto child = f.service.fork();
    CHECK(child && f.service.activate(child.value()));
    CHECK(f.service.fcntl(alias.value(), posix::f_SETFL, posix::o_NONBLOCK | posix::o_RDWR));
    CHECK(f.service.fcntl(reader, posix::f_GETFL, 0).value() == posix::o_NONBLOCK);
    CHECK(f.service.close(writer));
    CHECK(f.service.activate(f.pid));
    CHECK(f.service.fcntl(reader, posix::f_GETFL, 0).value() == posix::o_NONBLOCK);
    CHECK(f.service.write(writer, bytes("data")).value() == 4);
    CHECK(f.service.close(writer));
    f.ready(reader, Events::readable | Events::hangup);
    f.ready(reader, Events::hangup, Events::none);
    std::array<std::byte, 8> data{};
    CHECK(f.service.read(alias.value(), data).value() == 4 && data[0] == std::byte{'d'});
    f.ready(reader, Events::readable | Events::hangup);
    CHECK(f.service.read(reader, data).value() == 0);
    f.running();
    CHECK(f.service.close(reader) && f.service.close(alias.value()));
    CHECK(f.service.activate(child.value()));
    CHECK(f.service.exit(0));
    CHECK(f.service.activate(f.pid) && f.service.wait(f.pid, child.value()));
    CHECK(f.scheduler.open_description_count() == 3);
}
void description_pins_and_fd_reuse()
{
    static Fixture f;
    auto pair = f.service.pipe(posix::o_NONBLOCK);
    CHECK(pair);
    const auto reader = pair.value()[0], writer = pair.value()[1];
    const auto read_id = f.description(reader), write_id = f.description(writer);
    const auto pipe_id = f.scheduler.open_description(read_id)->pipe;
    CHECK(f.scheduler.retain_description(read_id) && f.scheduler.retain_description(write_id));
    CHECK(f.service.close(reader) && f.service.close(writer));
    auto replacement = f.service.open("/file", posix::o_RDONLY);
    CHECK(replacement && replacement.value() == reader && f.description(reader) != read_id);
    auto held = f.service.ready_description(read_id, io_events);
    CHECK(held && held.value().events == Events::none);
    f.ready(reader, io_events);
    CHECK(f.scheduler.write_pipe(pipe_id, bytes("held"), true).value() == 4);
    held = f.service.ready_description(read_id, io_events);
    CHECK(held && held.value().events == Events::readable);
    CHECK(f.scheduler.release_description(write_id));
    held = f.service.ready_description(read_id, io_events);
    CHECK(held && held.value().events == (Events::readable | Events::hangup));
    CHECK(f.scheduler.pipe_readiness(pipe_id).value().readers);
    CHECK(f.scheduler.release_description(read_id));
    CHECK(!f.scheduler.pipe_readiness(pipe_id));
    CHECK(!f.scheduler.release_description(read_id) && !f.scheduler.retain_description(read_id));
    held = f.service.ready_description(read_id, io_events);
    CHECK(held && held.value().events == Events::invalid);
    CHECK(f.service.close(replacement.value()));
    f.ready(reader, Events::invalid, Events::none);
    f.ready(-1, Events::invalid);
    f.ready(static_cast<posix::Fd>(sched::max_process_fds), Events::invalid);
    // A pin of the read endpoint also keeps a writer from seeing EPIPE.
    pair = f.service.pipe(posix::o_NONBLOCK);
    CHECK(pair);
    const auto pinned_reader = f.description(pair.value()[0]);
    CHECK(f.scheduler.retain_description(pinned_reader) && f.service.close(pair.value()[0]));
    f.ready(pair.value()[1], Events::writable);
    CHECK(f.scheduler.release_description(pinned_reader));
    f.ready(pair.value()[1], Events::error);
    CHECK(f.service.close(pair.value()[1]) && f.scheduler.open_description_count() == 3);
}
void synchronous_files_console_and_blocking_default()
{
    static Fixture f;
    auto file = f.service.open("/file", posix::o_RDONLY | posix::o_NONBLOCK);
    CHECK(file);
    f.ready(file.value(), io_events); // Read-only access errors also do not block.
    CHECK(f.service.seek(file.value(), 4096, posix::SeekWhence::set));
    f.ready(file.value(), Events::readable, Events::readable);
    f.ready(1, Events::writable);
    f.ready(2, Events::none, Events::readable);
    auto input = f.service.duplicate(0);
    CHECK(input);
    auto input_state = f.service.ready_events(input.value(), Events::readable);
    CHECK(input_state && input_state.value().console_input && input_state.value().events == Events::none);
    CHECK(f.service.close(file.value()) && f.service.close(input.value()));
    auto pair = f.service.pipe();
    CHECK(pair);
    CHECK(f.service.write(pair.value()[1], bytes("data")).value() == 4);
    std::array<std::byte, 8> scattered{};
    const posix::IoVector vectors[] = {{reinterpret_cast<uptr>(scattered.data()), 4},
                                       {reinterpret_cast<uptr>(scattered.data() + 4), 4}};
    CHECK(f.service.readv(pair.value()[0], vectors).value() == 4);
    CHECK(std::memcmp(scattered.data(), "data", 4) == 0);
    f.running();
    std::array<std::byte, 1> data{};
    CHECK(f.service.read(pair.value()[0], data).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.find(f.pid)->state() == sched::ProcessState::blocked);
    // Peer close wakes a real blocking wait; nonblocking tests above never arm it.
    CHECK(f.service.close(pair.value()[1]));
    CHECK(f.scheduler.find(f.pid)->state() == sched::ProcessState::runnable);
    CHECK(f.service.activate(f.pid));
    CHECK(f.service.read(pair.value()[0], data).value() == 0);
    CHECK(f.service.close(pair.value()[0]));
}
void capacity_failure_is_atomic()
{
    static Fixture f;
    std::array<posix::Fd, sched::max_process_fds - 4> aliases{};
    for (auto &fd : aliases)
    {
        auto copied = f.service.duplicate(1);
        CHECK(copied);
        fd = copied.value();
    }
    CHECK(f.service.open_file_count() == sched::max_process_fds - 1);
    const auto descriptions = f.scheduler.open_description_count();
    CHECK(!f.service.pipe(posix::o_NONBLOCK | posix::o_CLOEXEC));
    CHECK(f.service.open_file_count() == sched::max_process_fds - 1);
    CHECK(f.scheduler.open_description_count() == descriptions);
    for (const auto fd : aliases)
        CHECK(f.service.close(fd));
    for (usize i = 0; i < sched::max_process_pipes * 2; ++i)
    {
        auto pair = f.service.pipe(posix::o_NONBLOCK | posix::o_CLOEXEC);
        CHECK(pair);
        CHECK(f.service.close(pair.value()[0]) && f.service.close(pair.value()[1]));
    }
    CHECK(f.scheduler.open_description_count() == 3);
}
void node_pin_survives_unlink()
{
    static Fixture f;
    CHECK(f.vfs.write_file("/file", bytes("original")));
    auto old = f.service.open("/file", posix::o_RDONLY);
    CHECK(old);
    const auto held = f.description(old.value());
    CHECK(f.scheduler.retain_description(held));
    CHECK(f.service.unlink("/file") && f.service.close(old.value()));
    CHECK(f.vfs.create("/file", fs::NodeType::regular));
    CHECK(f.vfs.write_file("/file", bytes("replacement")));
    auto replacement = f.service.open("/file", posix::o_RDONLY);
    CHECK(replacement && replacement.value() == old.value() && f.description(replacement.value()) != held);
    auto *description = f.scheduler.open_description(held);
    CHECK(description && description->node && description->references == 1);
    auto contents = description->node->read(0, 16);
    CHECK(contents && contents.value().size == 8 && std::memcmp(contents.value().data.data(), "original", 8) == 0);
    CHECK(f.service.ready_description(held, io_events).value().events == io_events);
    CHECK(f.scheduler.release_description(held));
    std::array<std::byte, 16> buffer{};
    CHECK(f.service.read(replacement.value(), buffer).value() == 11);
    CHECK(std::memcmp(buffer.data(), "replacement", 11) == 0);
    CHECK(f.service.close(replacement.value()) && f.scheduler.open_description_count() == 3);
}
int main()
{
    nonblocking_and_atomic_writes();
    shared_status_and_peer_close();
    description_pins_and_fd_reuse();
    synchronous_files_console_and_blocking_default();
    capacity_failure_is_atomic();
    node_pin_survives_unlink();
    std::puts("PASS descriptor readiness model: nonblocking state, PIPE_BUF, shared flags, close/reuse pins, "
              "EOF/HUP/ERR, console ownership");
}
