#include <ok/net/tcp_stream_checked.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
using namespace ok::net;
namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { std::fprintf(stderr, "TCP_ACCEPT_FAILED_HOLDER_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while(false)
constexpr tcp::Endpoint server{{10, 0, 0, 2}, 8080}, peer{{10, 0, 0, 3}, 32001};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(), length}; }
};
Packet packet(tcp::Endpoint endpoint, u32 seq, u32 ack, u8 flags)
{
    Packet out; auto built = wire::build_tcp(out.bytes, endpoint.address, server.address, 1, 64,
        {endpoint.port, server.port, seq, ack, flags, 4096}, {}, {});
    CHECK(built); out.length = built.value(); return out;
}
struct Fixture
{
    sched::Scheduler scheduler{};
    NativeTcpStreamService service{tcp::TableIdentity{61990}};
    TcpStreamOperations operations{scheduler, service};
    std::optional<PreparedTcpAccept> operation{};
};
struct Iss
{
    Fixture &owner; bool destroy{};
    Result<u32> operator()(tcp::Endpoint, tcp::Endpoint)
    {
        if (destroy)
        {
            // An actual ISS provider boundary refuses endpoint cancellation.
            // Destruction must preserve every held slot and process guard.
            CHECK(owner.operation->retire().code() == StatusCode::busy);
            CHECK(!owner.operations.status());
        }
        return u32{500};
    }
};
}
int main()
{
    auto owner = std::make_unique<Fixture>(); auto &f = *owner;
    CHECK(f.scheduler.set_native_tcp_service(f.service));
    const auto context = arch::configured_arch_operations().make_kernel_context(4096, 32768);
    auto created = f.scheduler.create_process("failed-holder", context); CHECK(created); const auto pid = created.value();
    auto opened = f.operations.open(pid, true); CHECK(opened); const auto fd = opened.value();
    auto *process = f.scheduler.find(pid); const auto listener_id = process->fd_table().entry(fd)->description;
    CHECK(f.operations.bind(pid, fd, server)); CHECK(f.operations.listen(pid, fd, 2));
    Iss iss{f}; CHECK(f.service.endpoints().ingest(packet(peer, 10, 0, wire::tcp_syn).view(), 0, iss));
    Packet response; auto prepared = f.service.endpoints().prepare(response.bytes, 0); CHECK(prepared && prepared.value());
    response.length = prepared.value()->length; CHECK(f.service.endpoints().complete(prepared.value()->ticket, true, 0));
    auto ip = wire::ipv4(response.view()); CHECK(ip); auto tcp = wire::tcp(ip.value()); CHECK(tcp);
    CHECK(f.service.endpoints().ingest(packet(peer, 11, tcp.value().fields.sequence + 1, wire::tcp_ack).view(), 1, iss));
    auto accepted = f.operations.prepare_accept(pid, fd); CHECK(accepted);
    const auto reserved_fd = accepted.value().descriptor();
    const auto token = process->fd_table().entry(reserved_fd)->reservation;
    const u16 reserved_id = [&] {
        for (u16 id = 3; id < sched::max_open_descriptions; ++id)
        {
            if (const auto *open = f.scheduler.open_description(id); open && open->reservation == token) return id;
        }
        return sched::invalid_description;
    }();
    CHECK(token && reserved_id != sched::invalid_description);
    f.operation.emplace(std::move(accepted.value())); iss.destroy = true;
    auto other_peer = peer; ++other_peer.port;
    auto incoming = f.service.endpoints().ingest(packet(other_peer, 20, 0, wire::tcp_syn).view(), 2, iss);
    CHECK(incoming && incoming.value().matched && f.operation);
    // The provider callback has ended. Neither explicit retry nor destruction
    // may discharge a holder whose first cleanup already failed.
    CHECK(f.operation->retire().code() == StatusCode::busy); CHECK(!f.operation->commit());
    f.operation.reset(); CHECK(!f.operation);
    CHECK(f.operations.status().code() == StatusCode::busy && f.service.prepared_accepts() == 1);
    CHECK(f.service.endpoints().pools().accept_holds == 1 && f.service.endpoints().pools().accept_listener_holds == 1);
    CHECK(process->resources().prepared_descriptor_operations == 1 && !process->fd_table().entry(reserved_fd)->used);
    CHECK(process->fd_table().entry(reserved_fd)->reservation == token && f.scheduler.open_description(reserved_id)->reservation == token);
    CHECK(f.scheduler.open_description(listener_id)->references == 2);
    CHECK(f.scheduler.drain_all_resources().code() == StatusCode::busy);
    CHECK(!f.scheduler.fork_process(pid)); CHECK(f.scheduler.replace_image(pid, context, {}).code() == StatusCode::busy);
    CHECK(f.scheduler.exit_process(pid, 0).code() == StatusCode::busy && f.scheduler.reap_process(pid).code() == StatusCode::busy);
    CHECK(!f.scheduler.install_descriptor(process->resources(), reserved_fd, listener_id));
    CHECK(!f.scheduler.retain_description(reserved_id)); CHECK(!f.operations.open(pid));
    CHECK(f.scheduler.close_descriptor(process->resources(), fd)); CHECK(f.scheduler.open_description(listener_id)->references == 1);
    CHECK(f.service.endpoints().retire()); CHECK(f.service.endpoints().pools().accept_holds == 1);
    CHECK(process->fd_table().entry(reserved_fd)->reservation == token && f.scheduler.drain_all_resources().code() == StatusCode::busy);
    std::printf("TCP_ACCEPT_FAILED_HOLDER_PASS checks=%zu preserved_diagnostic_holder=1 automatic_retry=0 syscall_execution=0 device_execution=0\n", checks);
}
