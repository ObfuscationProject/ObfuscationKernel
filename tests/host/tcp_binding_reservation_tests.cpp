#include <ok/net/native_tcp_stream.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;
using namespace ok::net;
namespace
{
usize checks{};
u64 identity{61000};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { std::fprintf(stderr, "TCP_BINDING_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while (false)
constexpr tcp::Endpoint local{{192, 0, 2, 2}, 32001}, peer{{192, 0, 2, 1}, 8080};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(), length}; }
    wire::Tcp parsed() const
    { auto ip = wire::ipv4(view()); CHECK(ip); auto tcp = wire::tcp(ip.value()); CHECK(tcp); return tcp.value(); }
};
Packet packet(tcp::Endpoint from, tcp::Endpoint to, u32 seq, u32 ack, u8 flags)
{
    Packet out;
    auto result = wire::build_tcp(out.bytes, from.address, to.address, 1, 64,
        {from.port, to.port, seq, ack, flags, 4096}, {}, {});
    CHECK(result); out.length = result.value(); return out;
}
struct Iss { u32 next{700}; Result<u32> operator()(tcp::Endpoint, tcp::Endpoint) { return next++; } };
struct Fixture
{
    NativeTcpStreamService service{tcp::TableIdentity{identity++}};
    Iss iss{};
    TcpStreamHandle open()
    { auto result = service.open(); CHECK(result); return result.value(); }
    void ingress(const Packet &bytes, u64 now)
    { auto result = service.endpoints().ingest(bytes.view(), now, iss); CHECK(result && result.value().matched); }
    Packet take(u64 now)
    {
        Packet out; auto prepared = service.endpoints().prepare(out.bytes, now); CHECK(prepared && prepared.value());
        out.length = prepared.value()->length;
        CHECK(service.endpoints().complete(prepared.value()->ticket, true, now)); return out;
    }
    TcpStreamHandle establish()
    {
        auto stream = open(); CHECK(service.bind(stream, local)); CHECK(service.connect(stream, peer, 100, 0));
        auto syn = take(0); CHECK(syn.parsed().fields.sequence == 100);
        ingress(packet(peer, local, 200, 101, wire::tcp_syn | wire::tcp_ack), 1); take(1);
        CHECK(service.readiness(stream).value().connect_complete); return stream;
    }
    void refused(TcpStreamHandle stream, tcp::Endpoint endpoint = local)
    {
        const auto before = service.snapshot(stream); CHECK(before);
        auto result = service.bind(stream, endpoint); CHECK(!result && result.code() == StatusCode::already_exists);
        const auto after = service.snapshot(stream); CHECK(after);
        CHECK(after.value().local == before.value().local && after.value().endpoint == before.value().endpoint);
    }
};
std::unique_ptr<Fixture> fixture() { return std::make_unique<Fixture>(); }

void bound_owners()
{
    auto owner = fixture(); auto &f = *owner;
    const auto first = f.open(), other = f.open(), distinct_address = f.open();
    CHECK(f.service.bind(first, local)); f.refused(other);
    CHECK(!f.service.bind(first, local));
    CHECK(f.service.bind(other, {local.address, static_cast<u16>(local.port + 1)}));
    CHECK(f.service.bind(distinct_address, {{192, 0, 2, 3}, local.port}));
    CHECK(f.service.close(first)); CHECK(!f.service.valid(first));
    const auto replacement = f.open(); CHECK(f.service.bind(replacement, local));
    CHECK(replacement.slot == first.slot && replacement.generation > first.generation);
    CHECK(!f.service.bind(first, {{192, 0, 2, 4}, local.port}));
    CHECK(f.service.close(replacement)); CHECK(f.service.close(other)); CHECK(f.service.close(distinct_address));
    CHECK(f.service.active_streams() == 0);
}

void failed_start_preserves_binding()
{
    auto owner = fixture(); auto &f = *owner;
    const auto bound = f.open(), contender = f.open(); CHECK(f.service.bind(bound, local));
    CHECK(!f.service.listen(bound, 0)); f.refused(contender);
    CHECK(!f.service.connect(bound, {{}, peer.port}, 100, 0)); f.refused(contender);
    CHECK(!f.service.snapshot(bound).value().endpoint);
    CHECK(f.service.close(bound)); CHECK(f.service.bind(contender, local)); CHECK(f.service.close(contender));
    const auto unbound = f.open(); CHECK(!f.service.bind(unbound, {{}, local.port}));
    CHECK(!f.service.bind(unbound, {local.address, 0}));
    CHECK(!f.service.bind(unbound, {{224, 0, 0, 1}, local.port}));
    CHECK(f.service.snapshot(unbound).value().local.port == 0); CHECK(f.service.close(unbound));
}

void direct_protocol_owners_and_selected_packet()
{
    auto owner = fixture(); auto &f = *owner; const auto contender = f.open();
    auto listener = f.service.endpoints().listen(local, 1); CHECK(listener); f.refused(contender);
    CHECK(f.service.endpoints().release(listener.value(), 0)); CHECK(!f.service.endpoints().local_reserved(local).value());
    auto connection = f.service.endpoints().connect(local, peer, 100, 0); CHECK(connection); f.refused(contender);
    Packet out; auto prepared = f.service.endpoints().prepare(out.bytes, 0); CHECK(prepared && prepared.value());
    CHECK(f.service.endpoints().release(connection.value(), 0));
    CHECK(!f.service.endpoints().state(connection.value())); CHECK(f.service.endpoints().pools().prepared);
    CHECK(f.service.endpoints().local_reserved(local).value()); f.refused(contender);
    CHECK(f.service.endpoints().complete(prepared.value()->ticket, false, 0));
    CHECK(f.service.endpoints().pools().connections == 0 && !f.service.endpoints().local_reserved(local).value());
    CHECK(f.service.bind(contender, local)); CHECK(f.service.close(contender));
}

void graceful_close_and_time_wait()
{
    auto owner = fixture(); auto &f = *owner; const auto connected = f.establish();
    const auto contender = f.open(); f.refused(contender);
    CHECK(f.service.close(connected)); CHECK(f.service.active_streams() == 1);
    CHECK(f.service.endpoints().pools().retiring == 1); f.refused(contender);
    const auto fin = f.take(2).parsed().fields; CHECK(fin.flags == (wire::tcp_fin | wire::tcp_ack));
    f.ingress(packet(peer, local, 201, fin.sequence + 1, wire::tcp_fin | wire::tcp_ack), 6); f.take(6);
    CHECK(f.service.endpoints().tick(2 + NativeTcpStreamService::Table::retirement_ms));
    CHECK(f.service.endpoints().pools().connections == 1); f.refused(contender);
    constexpr u64 expiry = 6 + 2 * tcp::Connection<>::msl_ms;
    CHECK(f.service.endpoints().tick(expiry - 1)); f.refused(contender);
    CHECK(f.service.endpoints().tick(expiry)); CHECK(f.service.endpoints().pools().connections == 0);
    CHECK(f.service.bind(contender, local)); CHECK(f.service.close(contender));
}

void accepted_children_share_listener_binding()
{
    auto owner = fixture(); auto &f = *owner;
    const auto listener = f.open(), contender = f.open();
    CHECK(f.service.bind(listener, local)); CHECK(f.service.listen(listener, 2));
    const tcp::Endpoint peer2{peer.address, static_cast<u16>(peer.port + 1)};
    f.ingress(packet(peer, local, 200, 0, wire::tcp_syn), 0); f.refused(contender);
    auto synack = f.take(0).parsed().fields;
    f.ingress(packet(peer, local, 201, synack.sequence + 1, wire::tcp_ack), 1);
    f.ingress(packet(peer2, local, 300, 0, wire::tcp_syn), 1);
    synack = f.take(1).parsed().fields;
    f.ingress(packet(peer2, local, 301, synack.sequence + 1, wire::tcp_ack), 2);
    const auto accepted1 = f.service.accept(listener), accepted2 = f.service.accept(listener);
    CHECK(accepted1 && accepted2); CHECK(f.service.snapshot(accepted1.value()).value().local == local);
    CHECK(f.service.snapshot(accepted2.value()).value().local == local);
    CHECK(f.service.endpoints().pools().unaccepted == 0);
    CHECK(f.service.close(listener)); CHECK(f.service.endpoints().pools().listeners == 0); f.refused(contender);
    const auto endpoint1 = *f.service.snapshot(accepted1.value()).value().endpoint;
    const auto endpoint2 = *f.service.snapshot(accepted2.value()).value().endpoint;
    CHECK(f.service.endpoints().abort(endpoint1, 2)); f.refused(contender);
    CHECK(f.service.close(accepted1.value())); f.refused(contender);
    CHECK(f.service.endpoints().abort(endpoint2, 2)); CHECK(f.service.close(accepted2.value()));
    CHECK(f.service.endpoints().pools().connections == 0); CHECK(f.service.bind(contender, local));
    CHECK(f.service.close(contender));
}

void callback_and_terminal_guards()
{
    auto owner = fixture(); auto &f = *owner;
    const auto listener = f.open(), contender = f.open();
    CHECK(f.service.bind(listener, local)); CHECK(f.service.listen(listener, 1));
    struct ClosingIss
    {
        Fixture &fixture; TcpStreamHandle listener, contender;
        Result<u32> operator()(tcp::Endpoint, tcp::Endpoint)
        {
            CHECK(fixture.service.close(listener)); CHECK(fixture.service.pending_closes() == 1);
            auto status = fixture.service.bind(contender, local); CHECK(!status && status.code() == StatusCode::busy);
            status = fixture.service.bind(contender, {local.address, static_cast<u16>(local.port + 1)});
            CHECK(!status && status.code() == StatusCode::busy);
            CHECK(fixture.service.snapshot(contender).value().local.port == 0); return u32{700};
        }
    } source{f, listener, contender};
    auto result = f.service.endpoints().ingest(packet(peer, local, 200, 0, wire::tcp_syn).view(), 1, source);
    CHECK(result && result.value().allocated); CHECK(f.service.pending_closes() == 1);
    f.refused(contender); CHECK(f.service.drain_closes()); CHECK(!f.service.endpoints().local_reserved(local).value());
    CHECK(f.service.bind(contender, local)); CHECK(f.service.close(contender));
    const auto terminal = f.open(); CHECK(f.service.endpoints().retire());
    const auto status = f.service.bind(terminal, local); CHECK(!status && status.code() == StatusCode::io_error);
    CHECK(f.service.snapshot(terminal).value().local.port == 0); CHECK(f.service.close(terminal));
}
}
int main()
{
    bound_owners(); failed_start_preserves_binding(); direct_protocol_owners_and_selected_packet();
    graceful_close_and_time_wait(); accepted_children_share_listener_binding(); callback_and_terminal_guards();
    std::printf("TCP_BINDING_HOSTED_PASS checks=%zu guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
