#include <ok/posix/posix.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
using namespace ok;
namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "UNIX_PATH_OWNER_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while(false)
static_assert(!std::is_copy_constructible_v<sched::UnixAccept>);
static_assert(!std::is_copy_assignable_v<sched::UnixAccept>);
static_assert(std::is_nothrow_move_constructible_v<sched::UnixAccept>);
constexpr sched::UnixPeerCredentials server_credential{41, 1001, 1002}, client_credential{42, 2001, 2002};
std::span<const std::byte> bytes(std::string_view value)
{ return {reinterpret_cast<const std::byte *>(value.data()), value.size()}; }
struct Fixture
{
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    fs::ResolveContext context{.credentials = {0, 0}, .check_search = true};
    Fixture() { CHECK(console.start()); CHECK(posix.initialize(vfs, console, scheduler)); }
    ~Fixture()
    {
        CHECK(scheduler.drain_all_resources());
        CHECK(scheduler.unix_endpoint_count() == 0);
        CHECK(scheduler.open_description_count() == 0);
    }
    sched::ProcessResources &resources() { return scheduler.boot_resources(); }
    u16 id(i32 fd) { return resources().files.entry(fd)->description; }
    i32 socket(bool cloexec = false, u32 flags = posix::o_NONBLOCK)
    { auto opened = scheduler.create_unix_socket(resources(), cloexec, flags); CHECK(opened); return opened.value(); }
    i32 listener(std::string_view path, usize backlog = 4)
    {
        const auto fd = socket(true);
        CHECK(scheduler.validate_unix_bind(id(fd)));
        auto node = vfs.create_socket(context, path, 0700); CHECK(node);
        CHECK(scheduler.bind_unix_socket(id(fd), std::move(node.value())));
        CHECK(!node.value().valid());
        CHECK(scheduler.listen_unix_socket(id(fd), backlog, server_credential));
        return fd;
    }
    void connect(i32 client, std::string_view path)
    {
        auto target = vfs.resolve_socket(context, path); CHECK(target);
        CHECK(scheduler.connect_unix_socket(id(client), target.value(), client_credential));
        CHECK(target.value().release());
    }
};
template<class Function> void run(Function function)
{
    // This is a hosted ownership model, not hardware or address-space execution.
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    function(fixture); fixture.~Fixture();
}
void queued_data_rights_and_accept_cancellation()
{
    run([](Fixture &f) {
        const auto listener = f.listener("/tmp/queued.sock");
        const auto client = f.socket();
        CHECK(!f.scheduler.unix_socket_readiness(f.id(listener)).value().readable);
        CHECK(f.scheduler.unix_peer_credentials(f.id(client)).status().code() == StatusCode::not_found);
        f.connect(client, "/tmp/queued.sock");
        CHECK(f.scheduler.unix_endpoint_count() == 3);
        CHECK(f.scheduler.unix_socket_readiness(f.id(listener)).value().readable);
        CHECK(f.scheduler.unix_peer_credentials(f.id(client)).value() == server_credential);
        CHECK(f.scheduler.listen_unix_socket(f.id(listener), 4, {99, 7, 8}));
        CHECK(f.vfs.create("/tmp/rights-data", fs::NodeType::regular));
        CHECK(f.vfs.write_file("/tmp/rights-data", bytes("abcdefgh")));
        auto opened = f.posix.open("/tmp/rights-data", posix::o_RDWR); CHECK(opened);
        const auto file = opened.value(); const auto file_id = f.id(file);
        std::array<i32, 1> rights{file};
        CHECK(f.posix.send_unix_rights(client, bytes("preaccept"), rights).value() == 9);
        CHECK(f.posix.close(file));
        CHECK(f.scheduler.collect_unix_rights());
        CHECK(f.scheduler.open_description(file_id)->unix_queue_references == 1);
        u16 queued{}; i32 reserved{};
        {
            auto accepted = f.scheduler.prepare_unix_accept(f.resources(), f.id(listener), true, posix::o_NONBLOCK);
            CHECK(accepted); queued = accepted.value().description(); reserved = accepted.value().descriptor();
            CHECK(accepted.value().peer_credentials() == client_credential);
            CHECK(f.resources().files.contains(reserved) && !f.resources().files.entry(reserved)->used);
            CHECK(f.scheduler.drain_resources(f.resources()).code() == StatusCode::busy);
            CHECK(f.scheduler.prepare_unix_accept(f.resources(), f.id(listener)).status().code() == StatusCode::busy);
            sched::UnixAccept moved{std::move(accepted.value())};
            CHECK(!accepted.value().commit()); moved.cancel(); moved.cancel();
        }
        CHECK(!f.resources().files.contains(reserved));
        CHECK(f.scheduler.open_description(queued)->references == 1);
        CHECK(f.scheduler.open_description(queued)->unix_queue_references == 1);
        CHECK(f.scheduler.unix_socket_readiness(f.id(listener)).value().readable);
        auto accepted = f.scheduler.prepare_unix_accept(f.resources(), f.id(listener), true, posix::o_NONBLOCK);
        CHECK(accepted && accepted.value().description() == queued);
        auto committed = accepted.value().commit(); CHECK(committed && committed.value() == reserved);
        CHECK(!accepted.value().commit());
        CHECK(f.id(committed.value()) == queued);
        CHECK(f.posix.fcntl(committed.value(), posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
        CHECK(f.posix.fcntl(committed.value(), posix::f_GETFL, 0).value() & posix::o_NONBLOCK);
        CHECK(f.scheduler.open_description(queued)->references == 1 && !f.scheduler.open_description(queued)->unix_queue_references);
        CHECK(f.scheduler.unix_peer_credentials(queued).value() == client_credential);
        CHECK(!f.scheduler.open_description(queued)->node && !f.scheduler.open_description(f.id(client))->node);
        std::array<std::byte, 32> data{};
        auto received = f.posix.prepare_unix_receive(committed.value(), data, 1, false, true); CHECK(received);
        CHECK(received.value().bytes() == 9 && data[0] == std::byte{'p'});
        const auto received_file = received.value().descriptors()[0];
        CHECK(received.value().commit().value() == 9 && f.id(received_file) == file_id);
        CHECK(f.posix.read(received_file, {data.data(), 2}).value() == 2 && data[0] == std::byte{'a'});
        CHECK(f.posix.close(received_file));
        CHECK(f.posix.write(committed.value(), bytes("reply")).value() == 5);
        CHECK(f.posix.read(client, data).value() == 5 && data[0] == std::byte{'r'});
        CHECK(f.posix.close(committed.value()));
        CHECK(f.scheduler.unix_socket_readiness(f.id(client)).value().hangup);
        CHECK(f.posix.read(client, data).value() == 0);
        CHECK(f.posix.close(client)); CHECK(f.posix.close(listener));
        CHECK(f.vfs.unlink("/tmp/queued.sock")); CHECK(f.vfs.unlink("/tmp/rights-data"));
    });
}
void listener_close_drops_pending_payload_and_rights()
{
    run([](Fixture &f) {
        const auto listener = f.listener("/tmp/retire-pending.sock");
        const auto client = f.socket(); f.connect(client, "/tmp/retire-pending.sock");
        CHECK(f.vfs.create("/tmp/retire-file", fs::NodeType::regular));
        auto opened = f.posix.open("/tmp/retire-file", posix::o_RDWR); CHECK(opened);
        const auto file = opened.value(); const auto file_id = f.id(file);
        const std::array<i32, 1> rights{file};
        CHECK(f.posix.send_unix_rights(client, bytes("pending-data"), rights));
        CHECK(f.scheduler.open_description(file_id)->unix_queue_references == 1);
        CHECK(f.posix.close(file));
        CHECK(f.posix.close(listener));
        CHECK(!f.scheduler.open_description(file_id));
        CHECK(f.scheduler.unix_socket_readiness(f.id(client)).value().hangup);
        CHECK(f.posix.write(client, bytes("after-close")).status().code() == StatusCode::broken_pipe);
        CHECK(f.posix.close(client));
        CHECK(f.vfs.unlink("/tmp/retire-pending.sock")); CHECK(f.vfs.unlink("/tmp/retire-file"));
    });
}
void unlink_rebind_and_listener_retirement()
{
    run([](Fixture &f) {
        const auto old_listener = f.listener("/tmp/rebind.sock");
        auto old_identity = f.vfs.resolve_socket(f.context, "/tmp/rebind.sock"); CHECK(old_identity);
        const auto client = f.socket(); f.connect(client, "/tmp/rebind.sock");
        auto accepted = f.scheduler.prepare_unix_accept(f.resources(), f.id(old_listener)); CHECK(accepted);
        const auto server = accepted.value().commit().value();
        CHECK(f.vfs.unlink("/tmp/rebind.sock"));
        CHECK(f.vfs.resolve_socket(f.context, "/tmp/rebind.sock").status().code() == StatusCode::not_found);
        const auto new_listener = f.listener("/tmp/rebind.sock");
        auto new_identity = f.vfs.resolve_socket(f.context, "/tmp/rebind.sock"); CHECK(new_identity);
        CHECK(!old_identity.value().same_identity(new_identity.value()));
        CHECK(f.posix.close(old_listener));
        CHECK(f.posix.write(client, bytes("old"))); std::array<std::byte, 8> data{};
        CHECK(f.posix.read(server, data).value() == 3 && data[0] == std::byte{'o'});
        const auto next_client = f.socket(); f.connect(next_client, "/tmp/rebind.sock");
        CHECK(f.posix.close(new_listener));
        CHECK(f.scheduler.unix_socket_readiness(f.id(next_client)).value().hangup);
        CHECK(f.posix.write(next_client, bytes("closed")).status().code() == StatusCode::broken_pipe);
        const auto stale_client = f.socket();
        CHECK(f.scheduler.connect_unix_socket(f.id(stale_client), new_identity.value(), client_credential).code() == StatusCode::not_found);
        CHECK(old_identity.value().release()); CHECK(new_identity.value().release());
        CHECK(f.posix.close(stale_client)); CHECK(f.posix.close(next_client)); CHECK(f.posix.close(client)); CHECK(f.posix.close(server));
        CHECK(f.vfs.unlink("/tmp/rebind.sock"));
    });
}
void queue_pressure_and_plain_accept_flags()
{
    run([](Fixture &f) {
        const auto listener = f.listener("/tmp/pressure.sock", 1);
        const auto first = f.socket(), second = f.socket(); f.connect(first, "/tmp/pressure.sock");
        auto target = f.vfs.resolve_socket(f.context, "/tmp/pressure.sock"); CHECK(target);
        const auto endpoint_count = f.scheduler.unix_endpoint_count(), descriptions = f.scheduler.open_description_count();
        CHECK(f.scheduler.connect_unix_socket(f.id(second), target.value(), client_credential).code() == StatusCode::would_block);
        CHECK(f.scheduler.unix_endpoint_count() == endpoint_count && f.scheduler.open_description_count() == descriptions);
        while (f.posix.open_file_count() < sched::max_process_fds) CHECK(f.posix.duplicate(first));
        CHECK(f.scheduler.prepare_unix_accept(f.resources(), f.id(listener)).status().code() == StatusCode::process_file_limit);
        CHECK(f.posix.close(static_cast<i32>(sched::max_process_fds - 1)));
        auto accepted = f.scheduler.prepare_unix_accept(f.resources(), f.id(listener)); CHECK(accepted);
        const auto server = accepted.value().commit().value();
        CHECK(!(f.posix.fcntl(server, posix::f_GETFL, 0).value() & posix::o_NONBLOCK));
        CHECK(f.posix.fcntl(server, posix::f_GETFD, 0).value() == 0);
        CHECK(f.scheduler.connect_unix_socket(f.id(first), target.value(), client_credential).code() == StatusCode::busy);
        CHECK(f.scheduler.prepare_unix_accept(f.resources(), f.id(listener)).status().code() == StatusCode::would_block);
        CHECK(target.value().release());
        CHECK(f.vfs.unlink("/tmp/pressure.sock"));
    });
}
void pending_listener_rights_cycle_and_reuse()
{
    run([](Fixture &f) {
        for (usize attempt = 0; attempt < 24; ++attempt)
        {
            const auto listener = f.listener("/tmp/cycle.sock");
            const auto client = f.socket(); f.connect(client, "/tmp/cycle.sock");
            std::array<i32, 1> rights{listener};
            CHECK(f.posix.send_unix_rights(client, bytes("listener-cycle"), rights));
            CHECK(f.posix.close(listener));
            CHECK(f.posix.close(client));
            CHECK(f.scheduler.collect_unix_rights());
            CHECK(f.scheduler.unix_endpoint_count() == 0 && f.scheduler.open_description_count() == 3);
            CHECK(f.vfs.unlink("/tmp/cycle.sock"));
        }
        auto pair = f.posix.unix_socketpair(posix::o_NONBLOCK); CHECK(pair);
        CHECK(f.scheduler.is_unix_socket(f.id(pair.value()[0])));
        CHECK(f.scheduler.unix_peer_credentials(f.id(pair.value()[0])).status().code() == StatusCode::unsupported);
        CHECK(f.posix.close(pair.value()[0])); CHECK(f.posix.close(pair.value()[1]));
    });
}
void bounded_endpoints_and_prepared_process_lifetime()
{
    run([](Fixture &f) {
        const auto unbound = f.socket();
        CHECK(f.scheduler.is_unix_socket(f.id(unbound)));
        CHECK(f.posix.fstat(unbound).value().type == fs::NodeType::socket);
        std::array<std::byte, 1> byte{};
        CHECK(!f.posix.read(unbound, byte) && !f.posix.write(unbound, bytes("x")));
        CHECK(f.scheduler.listen_unix_socket(f.id(unbound), 1, server_credential).code() == StatusCode::invalid_argument);
        CHECK(f.posix.close(unbound));
        // An independent resource table fills global endpoint capacity while
        // the boot table still has available descriptor slots.
        sched::ProcessResources other{};
        std::array<i32, sched::max_unix_endpoints> sockets{};
        for (usize index = 0; index < sockets.size(); ++index)
        {
            auto opened = f.scheduler.create_unix_socket(other); CHECK(opened); sockets[index] = opened.value();
        }
        CHECK(f.scheduler.create_unix_socket(f.resources()).status().code() == StatusCode::system_file_limit);
        CHECK(f.scheduler.drain_resources(other)); CHECK(f.scheduler.unix_endpoint_count() == 0);
        const auto listener = f.listener("/tmp/lifetime.sock"), client = f.socket();
        f.connect(client, "/tmp/lifetime.sock");
        auto context = arch::configured_arch_operations().make_user_context({1, 2, 0});
        auto made = f.scheduler.create_user_process("accept-owner-model", context); CHECK(made);
        auto &resources = f.scheduler.find(made.value())->resources();
        auto accepted = f.scheduler.prepare_unix_accept(resources, f.id(listener)); CHECK(accepted);
        CHECK(f.scheduler.exit_process(made.value(), 0).code() == StatusCode::busy);
        CHECK(f.scheduler.reap_process(made.value()).code() == StatusCode::busy);
        CHECK(f.scheduler.replace_image(made.value(), {}, {}).code() == StatusCode::busy);
        accepted.value().cancel();
        CHECK(f.scheduler.exit_process(made.value(), 0)); CHECK(f.scheduler.reap_process(made.value()));
        CHECK(f.vfs.unlink("/tmp/lifetime.sock"));
    });
}
}
int main()
{
    queued_data_rights_and_accept_cancellation(); listener_close_drops_pending_payload_and_rights();
    unlink_rebind_and_listener_retirement();
    queue_pressure_and_plain_accept_flags(); pending_listener_rights_cycle_and_reuse();
    bounded_endpoints_and_prepared_process_lifetime();
    std::printf("UNIX_PATH_OWNER_HOSTED_PASS checks=%zu\n", checks);
}
