#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
using namespace ok;
namespace ok::sched
{
struct SchedulerIdentityTestAccess
{
    static u64 revision(SchedulerState &scheduler, u16 pipe, u64 value)
    { return std::exchange(scheduler.pipes_[pipe].revision, value); }
    static u64 token(SchedulerState &scheduler, u64 value)
    { return std::exchange(scheduler.next_unix_receive_token_, value); }
};
}
static usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<sched::UnixReceive>);
static_assert(!std::is_copy_assignable_v<sched::UnixReceive>);
static_assert(std::is_nothrow_move_constructible_v<sched::UnixReceive>);
std::span<const std::byte> bytes(std::string_view text)
{ return {reinterpret_cast<const std::byte *>(text.data()), text.size()}; }
struct Fixture
{
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    Fixture()
    {
        CHECK(console.start()); CHECK(posix.initialize(vfs, console, scheduler));
        CHECK(vfs.create("/data", fs::NodeType::regular));
        CHECK(vfs.write_file("/data", bytes("abcdefghij")));
    }
    ~Fixture() { static_cast<void>(scheduler.drain_all_resources()); }
    auto pair() { auto result = posix.unix_socketpair(posix::o_NONBLOCK); CHECK(result); return result.value(); }
    i32 file() { auto result = posix.open("/data", posix::o_RDWR); CHECK(result); return result.value(); }
    sched::ProcessResources &resources()
    { const auto pid = scheduler.current_pid(); return pid ? scheduler.find(pid)->resources() : scheduler.boot_resources(); }
    u16 id(i32 fd) { return resources().files.entry(fd)->description; }
};
class ReleaseNode final : public fs::Node
{
  public:
    std::string_view name() const override { return "release-node"; }
    fs::Metadata metadata() const override { return {}; }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
    Status retain() override { ++retains; return {}; }
    Status release() override { ++releases; if (callback) callback(context); return failure; }
    usize retains{}, releases{};
    void (*callback)(void *){};
    void *context{};
    Status failure{};
};
template<class Function> void run_case(Function function)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    function(fixture); fixture.~Fixture();
}
void shared_offset_and_sender_close()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 2> rights{fd, fd}; std::array<std::byte, 16> out{};
        CHECK(f.posix.read(fd, {out.data(), 2}).value() == 2);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("ok"), rights).value() == 2);
        CHECK(f.scheduler.open_description(id)->references == 3);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == 2);
        CHECK(f.posix.close(fd));
        auto receive = f.posix.prepare_unix_receive(pair[1], out, 8, false, true); CHECK(receive);
        const auto first = receive.value().descriptors()[0], second = receive.value().descriptors()[1];
        CHECK(!f.resources().files.entry(first)->used && f.resources().files.contains(first));
        CHECK(receive.value().bytes() == 2 && out[0] == std::byte{'o'});
        CHECK(receive.value().commit().value() == 2); CHECK(!receive.value().commit());
        CHECK(f.id(first) == id && f.id(second) == id);
        CHECK(f.posix.fcntl(first, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
        CHECK(f.posix.read(first, {out.data(), 2}).value() == 2 && out[0] == std::byte{'c'});
        CHECK(f.posix.read(second, {out.data(), 2}).value() == 2 && out[0] == std::byte{'e'});
        CHECK(f.scheduler.open_description(id)->references == 2 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.close(first)); CHECK(f.posix.close(second)); CHECK(!f.scheduler.open_description(id));
    });
}
void cancellation_move_and_stale_preview()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); std::array<i32, 1> rights{fd};
        const auto id = f.id(fd); std::array<std::byte, 8> out{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("abc"), rights));
        i32 reserved{};
        {
            auto receive = f.posix.prepare_unix_receive(pair[1], out, 1); CHECK(receive);
            reserved = receive.value().descriptors()[0];
            CHECK(f.posix.duplicate_to(fd, reserved).status().code() == StatusCode::busy);
            CHECK(f.scheduler.drain_resources(f.resources()).code() == StatusCode::busy);
            sched::UnixReceive moved{std::move(receive.value())};
            CHECK(!receive.value().commit()); moved.cancel(); moved.cancel();
        }
        CHECK(!f.resources().files.contains(reserved));
        CHECK(f.scheduler.open_description(id)->references == 2 && f.scheduler.open_description(id)->unix_queue_references == 1);
        auto receive = f.posix.prepare_unix_receive(pair[1], out, 1); CHECK(receive);
        CHECK(f.posix.write(pair[0], bytes("tail"))); // Invalidates preview without consuming anything.
        CHECK(receive.value().commit().status().code() == StatusCode::busy);
        CHECK(!f.resources().files.contains(reserved));
        auto retry = f.posix.prepare_unix_receive(pair[1], out, 1); CHECK(retry);
        CHECK(retry.value().bytes() == 3); CHECK(retry.value().commit().value() == 3);
        CHECK(f.posix.read(pair[1], out).value() == 4 && out[0] == std::byte{'t'});
    });
}
void peek_and_control_truncation()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 3> rights{fd, fd, fd}; std::array<std::byte, 8> out{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("abc"), rights));
        auto peek = f.posix.prepare_unix_receive(pair[1], out, 1, true); CHECK(peek);
        CHECK(peek.value().control_truncated() && peek.value().descriptors().size() == 1);
        const auto peek_fd = peek.value().descriptors()[0]; CHECK(peek.value().commit().value() == 3);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == 3);
        CHECK(f.posix.fcntl(peek_fd, posix::f_GETFD, 0).value() == 0);
        auto receive = f.posix.prepare_unix_receive(pair[1], out, 0); CHECK(receive);
        CHECK(receive.value().control_truncated() && receive.value().descriptors().empty());
        CHECK(receive.value().commit().value() == 3);
        CHECK(f.scheduler.open_description(id)->references == 2 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.close(peek_fd));
    });
}
void ordinary_read_and_readv_boundaries()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 1> rights{fd}; std::array<std::byte, 32> out{};
        CHECK(f.posix.write(pair[0], bytes("pre")));
        CHECK(f.posix.send_unix_rights(pair[0], bytes("abcd"), rights));
        CHECK(f.posix.send_unix_rights(pair[0], bytes("ef"), rights));
        CHECK(f.posix.read(pair[1], {out.data(), 3}).value() == 3);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == 2);
        std::array<posix::IoVector, 2> vectors{{{reinterpret_cast<uptr>(out.data()), 4},
            {reinterpret_cast<uptr>(out.data() + 4), 4}}};
        CHECK(f.posix.readv(pair[1], vectors).value() == 4);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == 1);
        CHECK(f.posix.read(pair[1], {out.data(), 1}).value() == 1);
        CHECK(!f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.read(pair[1], out).value() == 1);
    });
}
void pressure_and_failed_retention()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 1> rights{fd}; std::array<std::byte, 8> out{};
        for (usize index = 0; index < sched::max_unix_records; ++index)
            CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights).value() == 1);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("y"), rights).status().code() == StatusCode::would_block);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == sched::max_unix_records);
        CHECK(f.posix.send_unix_rights(pair[0], {}, rights).value() == 0);
        while (f.scheduler.open_description(id)->unix_queue_references) CHECK(f.posix.read(pair[1], out).value() == 1);
        auto *open = f.scheduler.open_description(id); const auto old = open->references;
        open->references = ~usize{0} - 1;
        std::array<i32, 2> duplicate{fd, fd};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("fail"), duplicate).status().code() == StatusCode::overflow);
        CHECK(open->references == ~usize{0} - 1 && !open->unix_queue_references); open->references = old;
        CHECK(f.posix.read(pair[1], out).status().code() == StatusCode::would_block);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights));
        open->references = ~usize{0};
        CHECK(f.posix.prepare_unix_receive(pair[1], out, 1).status().code() == StatusCode::overflow);
        CHECK(!f.resources().unix_receive_operations); open->references = old + 1;
        CHECK(f.posix.read(pair[1], out).value() == 1);
    });
}
void descriptor_pressure_and_peer_teardown()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 2> rights{fd, fd}; std::array<std::byte, 8> out{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights));
        while (f.posix.open_file_count() < sched::max_process_fds) CHECK(f.posix.duplicate(fd));
        auto receive = f.posix.prepare_unix_receive(pair[1], out, 8); CHECK(receive);
        CHECK(receive.value().descriptors().empty() && receive.value().control_truncated());
        CHECK(receive.value().commit().value() == 1 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights));
        CHECK(f.posix.close(pair[1])); CHECK(!f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights).status().code() == StatusCode::broken_pipe);
    });
}
void self_and_cross_socket_cycles()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto second = f.pair();
        const auto first_id = f.id(pair[1]), second_id = f.id(second[1]);
        std::array<i32, 1> first{pair[1]}, other{second[1]};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), first));
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), other));
        CHECK(f.posix.send_unix_rights(second[0], bytes("x"), first));
        CHECK(f.posix.close(pair[0])); CHECK(f.posix.close(second[0]));
        CHECK(f.posix.close(pair[1])); CHECK(f.scheduler.open_description(first_id));
        CHECK(f.posix.close(second[1]));
        CHECK(!f.scheduler.open_description(first_id) && !f.scheduler.open_description(second_id));
        CHECK(f.scheduler.open_description_count() == 3); CHECK(f.scheduler.collect_unix_rights());
        for (usize index = 0; index < sched::max_process_pipes / 2; ++index) CHECK(f.posix.unix_socketpair());
    });
}
void process_lifetime_guards()
{
    run_case([](Fixture &f) {
        auto process = f.scheduler.create_user_process("receiver", arch::configured_arch_operations().make_user_context({1, 2, 0})); CHECK(process);
        CHECK(f.posix.activate(process.value()));
        const auto pair = f.pair(); const auto fd = f.file(); std::array<i32, 1> rights{fd}; std::array<std::byte, 8> out{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights));
        auto receive = f.posix.prepare_unix_receive(pair[1], out, 1); CHECK(receive);
        CHECK(f.posix.fork().status().code() == StatusCode::busy);
        CHECK(f.scheduler.exit_process(process.value(), 0).code() == StatusCode::busy);
        CHECK(f.scheduler.reap_process(process.value()).code() == StatusCode::busy);
        CHECK(f.scheduler.replace_image(process.value(), {}, {}).code() == StatusCode::busy);
        receive.value().cancel(); CHECK(f.scheduler.exit_process(process.value(), 0)); CHECK(f.scheduler.reap_process(process.value()));
    });
}
void partial_send_invalid_admission_and_eof()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        std::array<i32, 1> rights{fd}; std::array<std::byte, sched::pipe_capacity> full{}, out{};
        CHECK(f.posix.write(pair[0], {full.data(), full.size() - 2}).value() == full.size() - 2);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("partial"), rights).value() == 2);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights).status().code() == StatusCode::would_block);
        CHECK(f.scheduler.open_description(id)->unix_queue_references == 1);
        CHECK(f.posix.read(pair[1], out).value() == full.size());
        CHECK(!f.scheduler.open_description(id)->unix_queue_references);
        std::array<i32, sched::max_unix_rights + 1> too_many{}; too_many.fill(fd);
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), too_many).status().code() == StatusCode::message_size);
        std::array<i32, 2> invalid{fd, -1};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), invalid).status().code() == StatusCode::bad_descriptor);
        CHECK(f.scheduler.open_description(id)->references == 1);
        CHECK(f.posix.send_unix_rights(fd, bytes("x"), rights).status().code() == StatusCode::unsupported);
        CHECK(f.posix.close(pair[0])); auto eof = f.posix.prepare_unix_receive(pair[1], out, 1); CHECK(eof);
        CHECK(eof.value().bytes() == 0 && eof.value().descriptors().empty()); CHECK(eof.value().commit().value() == 0);
    });
}
void release_failure_and_callback_reuse()
{
    run_case([](Fixture &f) {
        ReleaseNode node; const auto pair = f.pair();
        const auto description = f.scheduler.allocate_description({.readable = true, .node = &node}); CHECK(description);
        CHECK(f.scheduler.install_descriptor(f.resources(), 10, description.value()));
        std::array<i32, 1> rights{10}; std::array<std::byte, 8> out{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights)); CHECK(f.posix.close(10));
        struct Context { Fixture *fixture; u16 old; std::array<i32, 2> fresh{}; } context{&f, description.value()};
        node.context = &context; node.failure = Status::io_error("provider release failed after consuming hold");
        node.callback = [](void *raw) {
            auto &context = *static_cast<Context *>(raw); auto &f = *context.fixture;
            CHECK(!f.scheduler.open_description(context.old)); CHECK(f.scheduler.collect_unix_rights());
            context.fresh = f.pair(); CHECK(f.id(context.fresh[0]) == context.old);
        };
        CHECK(f.posix.read(pair[1], out).value() == 1); CHECK(node.retains == 1 && node.releases == 1);
        CHECK(f.scheduler.resource_drain_report().first_failure.code() == StatusCode::io_error);
        CHECK(f.posix.write(context.fresh[0], bytes("fresh")).value() == 5);
        CHECK(f.posix.read(context.fresh[1], out).value() == 5);
        CHECK(f.posix.close(pair[0])); CHECK(f.posix.close(pair[1]));
        CHECK(f.posix.close(context.fresh[0])); CHECK(f.posix.close(context.fresh[1]));
        CHECK(node.releases == 1);
    });
}
void multiple_prepares_and_exhausted_counters()
{
    run_case([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd);
        const auto pipe = f.scheduler.open_description(f.id(pair[1]))->pipe;
        std::array<i32, 1> rights{fd}; std::array<std::byte, 8> first{}, second{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("abc"), rights));
        auto a = f.posix.prepare_unix_receive(pair[1], first, 1); CHECK(a);
        auto b = f.posix.prepare_unix_receive(pair[1], second, 1); CHECK(b);
        CHECK(a.value().descriptors()[0] != b.value().descriptors()[0] && f.resources().unix_receive_operations == 2);
        const auto winner = a.value().descriptors()[0]; CHECK(a.value().commit().value() == 3);
        CHECK(b.value().commit().status().code() == StatusCode::busy && !f.resources().unix_receive_operations);
        CHECK(f.scheduler.open_description(id)->references == 2 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.close(winner)); CHECK(f.posix.send_unix_rights(pair[0], bytes("xyz"), rights));
        const auto revision = sched::SchedulerIdentityTestAccess::revision(f.scheduler, pipe, ~u64{0});
        CHECK(f.posix.send_unix_rights(pair[0], bytes("x"), rights).status().code() == StatusCode::overflow);
        CHECK(f.posix.read(pair[1], first).status().code() == StatusCode::overflow);
        CHECK(f.posix.prepare_unix_receive(pair[1], first, 1).status().code() == StatusCode::overflow);
        CHECK(f.scheduler.open_description(id)->references == 2 && f.scheduler.open_description(id)->unix_queue_references == 1);
        static_cast<void>(sched::SchedulerIdentityTestAccess::revision(f.scheduler, pipe, revision));
        const auto token = sched::SchedulerIdentityTestAccess::token(f.scheduler, ~u64{0} - 1);
        auto final = f.posix.prepare_unix_receive(pair[1], first, 1); CHECK(final);
        CHECK(f.posix.prepare_unix_receive(pair[1], second, 1).status().code() == StatusCode::overflow);
        final.value().cancel(); static_cast<void>(sched::SchedulerIdentityTestAccess::token(f.scheduler, token));
        CHECK(f.posix.read(pair[1], {first.data(), 1}).value() == 1);
        CHECK(!f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.read(pair[1], first).value() == 2);
    });
}
int main()
{
    shared_offset_and_sender_close(); cancellation_move_and_stale_preview(); peek_and_control_truncation();
    ordinary_read_and_readv_boundaries(); pressure_and_failed_retention(); descriptor_pressure_and_peer_teardown();
    self_and_cross_socket_cycles(); process_lifetime_guards();
    partial_send_invalid_admission_and_eof(); release_failure_and_callback_reuse();
    multiple_prepares_and_exhausted_counters();
    std::printf("UNIX_RIGHTS_HOSTED_PASS checks=%zu\n", checks);
}
