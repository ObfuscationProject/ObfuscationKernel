#include <ok/net/native_tcp_stream.hpp>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::net;
using namespace ok::net::tcp;
using namespace ok::net::wire;
namespace
{
usize checks{};
void check(bool value, const char *why)
{ ++checks; if (!value) { std::fprintf(stderr, "FAIL check=%zu %s\n", checks, why); std::exit(1); } }
constexpr Endpoint server{{10, 0, 0, 2}, 8080};
constexpr Endpoint first{{10, 0, 0, 3}, 32001}, second{first.address, 32002};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    Bytes view() const { return {bytes.data(), length}; }
    Tcp parsed() const
    { auto ip = ipv4(view()); check(bool(ip), "peer packet IPv4 checksum"); auto out = wire::tcp(ip.value()); check(bool(out), "peer packet TCP checksum"); return out.value(); }
};
Packet packet(Endpoint peer, u32 sequence, u32 acknowledgment, u8 flags)
{
    Packet out;
    auto built = build_tcp(out.bytes, peer.address, server.address, 1, 64,
        {peer.port, server.port, sequence, acknowledgment, flags, 4096}, {}, {});
    check(bool(built), "independent peer packet construction"); out.length = built.value(); return out;
}
struct Iss { u32 next{500}; Result<u32> operator()(Endpoint, Endpoint) { return next++; } };
template <class T> void ingest(T &table, Packet input, u64 now, Iss &iss)
{ auto result = table.ingest(input.view(), now, iss); check(bool(result) && result.value().matched, "actual endpoint ingress matched"); }
template <class T> Packet take(T &table, u64 now)
{
    Packet out; auto prepared = table.prepare(out.bytes, now);
    check(bool(prepared) && bool(prepared.value()), "actual endpoint output packet prepared");
    out.length = prepared.value()->length;
    check(bool(table.complete(prepared.value()->ticket, true, now)), "actual endpoint output completed");
    return out;
}
template <class T> void completed(T &table, Endpoint peer, u64 now, Iss &iss)
{
    ingest(table, packet(peer, 10, 0, tcp_syn), now, iss);
    const auto syn = take(table, now).parsed().fields;
    check(syn.destination_port == peer.port, "selected passive peer exact");
    ingest(table, packet(peer, 11, syn.sequence + 1, tcp_ack), now + 1, iss);
}
void queued_selection_and_exact_tokens()
{
    EndpointTable<4, 1, 4> table{{101}}; Iss iss; auto listener = table.listen(server, 4).value();
    ingest(table, packet(first, 10, 0, tcp_syn), 0, iss);
    check(!table.prepare_accept(listener) && table.pools().accept_holds == 0, "incomplete handshake cannot prepare accept");
    const auto syn = take(table, 0).parsed().fields;
    completed(table, second, 1, iss);
    ingest(table, packet(first, 11, syn.sequence + 1, tcp_ack), 3, iss);
    auto one = table.prepare_accept(listener), two = table.prepare_accept(listener);
    check(bool(one) && bool(two) && one.value().peer == second && two.value().peer == first,
        "preparation selects oldest complete unreserved children");
    const auto a = one.value().ticket, b = two.value().ticket;
    check(table.pools().accept_holds == 2 && table.pools().accept_listener_holds == 2 &&
        table.pools().unaccepted == 2 && table.readiness(listener).value().accept_count == 0 && !table.accept(listener),
        "prepared children retain queue ownership and disappear from accept readiness");
    auto forged = a; ++forged.queue_order;
    check(!table.validate_accept(forged) && !table.cancel_accept(forged) && table.pools().accept_holds == 2,
        "foreign queue order cannot retire exact prepared hold");
    forged = a; ++forged.operation;
    check(!table.commit_accept(forged) && !table.cancel_accept(forged), "foreign operation serial cannot commit or cancel");
    forged = a; ++forged.child.generation;
    check(!table.cancel_accept(forged), "foreign child generation cannot retire hold");
    check(bool(table.cancel_accept(a)) && table.pools().unaccepted == 2 && table.pools().accept_holds == 1 &&
        table.readiness(listener).value().accept_count == 1, "cancellation restores live queue readiness without consuming child");
    auto again = table.prepare_accept(listener).value();
    check(again.ticket.child == a.child && again.ticket.queue_order == a.queue_order && again.ticket.operation != a.operation,
        "cancelled oldest child retains completion order with fresh operation authority");
    check(!table.cancel_accept(a) && !table.commit_accept(a), "cancelled token cannot touch successor reservation");
    auto accepted_b = table.commit_accept(b), accepted_a = table.commit_accept(again.ticket);
    check(bool(accepted_a) && bool(accepted_b) && table.pools().accept_holds == 0 && table.pools().accept_listener_holds == 0 &&
        table.pools().unaccepted == 0 && table.state(accepted_a.value()).value().peer == second &&
        table.state(accepted_b.value()).value().peer == first, "commit transfers exactly two queued edges without allocation");
    check(!table.commit_accept(b) && !table.cancel_accept(b), "committed token replay refused");
    check(bool(table.release(listener, 3)), "listener release after accept");
    check(bool(table.abort(accepted_a.value(), 3)) && bool(table.release(accepted_a.value(), 3)) &&
        bool(table.abort(accepted_b.value(), 3)) && bool(table.release(accepted_b.value(), 3)) && table.pools().connections == 0,
        "accepted owners drain independently");
}
void reset_close_and_retirement_holds()
{
    for (u64 identity = 102; identity <= 104; ++identity)
    {
        EndpointTable<1, 1, 1> table{{identity}}; Iss iss; auto listener = table.listen(server, 1).value();
        completed(table, first, 0, iss); auto prepared = table.prepare_accept(listener).value();
        if (identity == 102) ingest(table, packet(first, 11, 501, tcp_rst | tcp_ack), 2, iss);
        if (identity != 104) check(bool(table.release(listener, 2)), "final listener close with exact prepared hold");
        else check(bool(table.retire()), "table retirement with exact prepared hold");
        check(!table.commit_accept(prepared.ticket) && table.pools().connections == 1 &&
            table.pools().accept_holds == 1 && table.pools().accept_listener_holds == 1 && table.pools().unaccepted == 0,
            "terminal protocol state drops queue edge but cannot reuse held generation");
        if (identity != 104)
        {
            check(!table.listen(server, 1), "closed listener slot cannot be reused while prepared operation holds it");
            check(!table.connect(server, second, 900, 2), "terminal child slot cannot be reused while held");
        }
        check(bool(table.cancel_accept(prepared.ticket)) && table.pools().connections == 0 &&
            table.pools().accept_holds == 0 && table.pools().accept_listener_holds == 0 && !table.pools().prepared,
            "exact cancellation drains terminal holders without resurrection");
        check(!table.cancel_accept(prepared.ticket), "terminal hold cannot be retired twice");
        if (identity != 104)
        { auto next = table.listen(server, 1); check(bool(next) && next.value().generation > listener.generation, "listener slot reusable only after hold retirement"); }
    }
}
void readiness_and_operation_limits()
{
    EndpointTable<1, 1, 1> serial{{105}, {~u64{0}, ~u64{0}, ~u64{0}, 65535, 1}};
    Iss iss; auto listener = serial.listen(server, 1).value(); completed(serial, first, 0, iss);
    auto prepared = serial.prepare_accept(listener).value(); check(bool(serial.cancel_accept(prepared.ticket)), "limited serial cancelled");
    auto refused = serial.prepare_accept(listener);
    check(!refused && refused.status().code() == StatusCode::overflow && serial.pools().accept_holds == 0 &&
        serial.readiness(listener).value().accept_count == 1, "operation serial never wraps and leaves queue owner intact");
    EndpointTable<1, 1, 1> readiness{{106}, {~u64{0}, 5, ~u64{0}, 65535}};
    listener = readiness.listen(server, 1).value(); completed(readiness, first, 0, iss);
    auto reservation = readiness.prepare_accept(listener);
    check(bool(reservation), "readiness reservation preflight permits bounded commit");
    ingest(readiness, packet(first, 11, 502, tcp_ack | tcp_fin), 2, iss);
    check(!readiness.commit_accept(reservation.value().ticket) && readiness.pools().accept_holds == 1 &&
        readiness.pools().unaccepted == 1, "readiness exhaustion refuses commit before ownership mutation");
    check(bool(readiness.cancel_accept(reservation.value().ticket)) && readiness.pools().accept_holds == 0,
        "readiness refusal can retire its exact hold");
}
void stream_slot_reservations()
{
    NativeTcpStreamService service{{107}, {~u64{0}, 2}}; Iss iss;
    auto listener = service.open().value(); check(bool(service.bind(listener, server)) && bool(service.listen(listener, 1)), "stream listener established");
    completed(service.endpoints(), first, 0, iss);
    auto pressure = service.open().value();
    check(!service.prepare_accept(listener) && service.endpoints().pools().unaccepted == 1 &&
        service.endpoints().pools().accept_holds == 0, "stream pressure refuses preparation before queue mutation");
    check(bool(service.close(pressure)), "unconnected pressure owner closed");
    auto prepared = service.prepare_accept(listener).value();
    check(!service.valid(prepared.stream) && service.prepared_accepts() == 1 && service.active_streams() == 2 &&
        !service.open(), "reserved stream is unpublished and unavailable to competing creation");
    auto forged = prepared; ++forged.endpoint.peer.port;
    check(!service.commit_accept(forged) && !service.cancel_accept(forged) && service.prepared_accepts() == 1,
        "forged immutable peer cannot inherit stream authority");
    forged = prepared; ++forged.listener.generation;
    check(!service.cancel_accept(forged), "foreign original stream generation cannot cancel reservation");
    check(bool(service.cancel_accept(prepared)) && service.prepared_accepts() == 0 &&
        service.endpoints().pools().unaccepted == 1, "stream cancellation returns vacant slot while preserving live queued child");
    auto successor = service.prepare_accept(listener).value();
    check(successor.stream.generation > prepared.stream.generation && !service.cancel_accept(prepared), "stream generations cannot reuse cancelled reservation authority");
    auto accepted = service.commit_accept(successor);
    check(bool(accepted) && service.valid(accepted.value()) && service.prepared_accepts() == 0 &&
        service.snapshot(accepted.value()).value().endpoint == successor.endpoint.ticket.child,
        "stream publication consumes prepared endpoint without second allocation");
    check(bool(service.close(listener)) && service.valid(accepted.value()), "accepted stream survives original listener close");
    check(bool(service.endpoints().abort(successor.endpoint.ticket.child, 1)) && bool(service.close(accepted.value())) &&
        service.active_streams() == 0 && service.endpoints().pools().connections == 0, "accepted stream and endpoint owners fully retire");
}
void stream_listener_reuse()
{
    NativeTcpStreamService service{{108}, {~u64{0}, 2}}; Iss iss;
    auto listener = service.open().value(); check(bool(service.bind(listener, server)) && bool(service.listen(listener, 1)), "reuse listener established");
    completed(service.endpoints(), first, 0, iss); auto prepared = service.prepare_accept(listener).value();
    check(bool(service.close(listener)), "original stream listener revoked with prepared operation");
    auto replacement = service.open().value();
    check(replacement.slot == listener.slot && replacement.generation > listener.generation && !service.commit_accept(prepared),
        "reinstalled numeric stream slot cannot inherit captured listener authority");
    check(bool(service.cancel_accept(prepared)) && service.valid(replacement) && service.endpoints().pools().connections == 0 &&
        service.prepared_accepts() == 0, "cancellation drains old holders without erasing replacement stream");
    check(bool(service.close(replacement)) && service.active_streams() == 0, "replacement stream survives until its own retirement");
}
}
int main()
{
    queued_selection_and_exact_tokens(); reset_close_and_retirement_holds(); readiness_and_operation_limits();
    stream_slot_reservations(); stream_listener_reuse();
    std::printf("OK_PREPARED_TCP_ACCEPT actual_source_hosted_model_checks=%zu syscall_execution=0 device_execution=0\n", checks);
}
