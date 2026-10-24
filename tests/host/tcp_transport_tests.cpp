#include <ok/net/tcp_transport.hpp>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace ok;
using namespace ok::net::tcp;
using namespace ok::net::wire;
namespace
{
usize checks{};
void check(bool pass, const char *message)
{ ++checks; if (!pass) { std::fprintf(stderr, "FAIL check=%zu %s\n", checks, message); std::exit(1); } }
constexpr Endpoint left{{10, 0, 0, 2}, 32001}, right{{10, 0, 0, 3}, 8080};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    Bytes view() const { return {bytes.data(), length}; }
    Tcp parsed() const { auto ip = ipv4(view()); check(bool(ip), "packet IPv4 checksum"); auto p = tcp(ip.value()); check(bool(p), "packet TCP checksum"); return p.value(); }
};
Packet peer_packet(TcpFields fields, Bytes payload = {}, Bytes options = {}, Endpoint from = right, Endpoint to = left)
{
    Packet p; fields.source_port = from.port; fields.destination_port = to.port;
    auto n = build_tcp(p.bytes, from.address, to.address, 10, 64, fields, options, payload);
    check(bool(n), "peer packet builder"); p.length = n.value(); return p;
}
template <typename C> Packet take(C &c, u64 now, bool accepted = true)
{
    Packet p; auto n = c.prepare(p.bytes, now); check(bool(n) && bool(n.value()), "transport emits expected packet");
    p.length = n.value()->length; check(bool(c.complete_transmit(n.value()->token, accepted, now)), "packet submission receipt"); return p;
}
template <typename C> void none(C &c, u64 now)
{ Packet p; auto n = c.prepare(p.bytes, now); check(bool(n) && !n.value(), "no unexpected transport packet"); }
template <typename C> void input(C &c, const Packet &p, u64 now)
{ check(bool(c.receive(p.view(), now)), "validated peer ingress"); }
template <typename C> void establish(C &c, u32 iss = 1000, u32 irs = 2000, u64 start = 0, u64 rtt = 100)
{
    check(bool(c.active_open(left, right, iss, start)), "active open");
    auto syn = take(c, start).parsed(); check(syn.fields.sequence == iss && syn.fields.flags == tcp_syn, "real active SYN");
    auto p = peer_packet({0, 0, irs, iss + 1, tcp_syn | tcp_ack, 60000}); input(c, p, start + rtt);
    check(c.snapshot().state == State::established && c.snapshot().send_next == iss + 1, "active three-way transition");
    auto ack = take(c, start + rtt).parsed(); check(ack.fields.flags == tcp_ack && ack.fields.acknowledgment == irs + 1, "final handshake ACK");
}
void serial_tests()
{
    static_assert(!std::is_copy_constructible_v<Connection<>> && !std::is_move_constructible_v<Connection<>>);
    for (u32 v : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu})
    {
        check(!before(v, v) && !after(v, v), "sequence reflexivity");
        for (u32 distance : {1u, 2u, 536u, 65535u, 0x7fffffffu})
        { check(before(v, v + distance), "sequence wrap before"); check(after(v + distance, v), "sequence wrap after"); }
        check(!before(v, v + 0x80000000u) && !after(v, v + 0x80000000u), "half-space ambiguity is not ordered");
        check(!between(v + 0x80000000u, v, v), "ambiguous point is not in zero-width interval");
    }
}
void ownership_and_handshake()
{
    Connection<32, 32, 16> c; check(bool(c.active_open(left, right, 0xffffffffu, 1)), "wrap active open");
    Packet p; auto prepared = c.prepare(p.bytes, 1); check(bool(prepared) && bool(prepared.value()), "prepared SYN");
    const u64 token = prepared.value()->token;
    check(!c.write(Bytes{}) && !c.shutdown_write() && !c.abort() && !c.tick(1), "pending token excludes mutation");
    check(!c.prepare(p.bytes, 1), "pending token cannot be copied");
    check(!c.complete_transmit(token + 1, true, 1), "foreign completion rejected");
    check(bool(c.complete_transmit(token, false, 1)), "rejected TX releases token");
    check(!c.complete_transmit(token, true, 1), "completion exactly once");
    check(c.snapshot().send_next == 0xffffffffu && !c.snapshot().flight, "rejected TX retains initial sequence");
    auto syn = take(c, 2); check(syn.parsed().fields.sequence == 0xffffffffu, "rejected TX retried without sequence loss");
    check(c.snapshot().send_next == 0, "SYN consumes wrapping sequence");
    auto bad = peer_packet({0, 0, 10, 1, tcp_syn | tcp_ack, 64}); input(c, bad, 3);
    auto rst = take(c, 3).parsed(); check(rst.fields.flags == tcp_rst && rst.fields.sequence == 1, "invalid SYN ACK gets reset");
    check(c.snapshot().state == State::syn_sent, "invalid ACK cannot establish");
    auto good = peer_packet({0, 0, 0xffffffffu, 0, tcp_syn | tcp_ack, 64}); input(c, good, 4);
    auto final = take(c, 4).parsed(); check(final.fields.sequence == 0 && final.fields.acknowledgment == 0, "both handshake sequences wrap");
    check(c.snapshot().state == State::established, "validated ACK establishes");
    auto stale = c.tick(3); check(!stale, "clock rollback rejected");
    std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto self = Bytes{reinterpret_cast<const std::byte *>(&c), sizeof(c)};
    auto mutable_self = std::span<std::byte>{reinterpret_cast<std::byte *>(&c), sizeof(c)};
    check(!c.write(self) && !c.read(mutable_self) && !c.prepare(mutable_self, 5), "self-aliases rejected without state mutation");
    auto w = c.write(data); check(bool(w) && w.value() == 4, "queued stream bytes"); data.fill(std::byte{9});
    auto tx_packet = take(c, 5); auto tx = tx_packet.parsed(); check(tx.payload.size() == 4 && byte(tx.payload, 0) == 1, "write copies user payload");
    check(c.snapshot().send_buffered == 4, "DMA completion is not ACK");
    auto invalid = peer_packet({0, 0, 0, 5, tcp_ack, 64}); input(c, invalid, 6);
    check(c.snapshot().send_buffered == 4, "future ACK cannot release retransmittable bytes");
    take(c, 6); auto ack = peer_packet({0, 0, 0, 2, tcp_ack, 64}); input(c, ack, 7);
    check(c.snapshot().send_buffered == 2 && c.snapshot().send_unacknowledged == 2, "partial ACK releases exact prefix");
    auto deadline = c.snapshot().retransmit_deadline; none(c, deadline - 1);
    auto retry_packet = take(c, deadline, false); auto retry = retry_packet.parsed(); check(retry.fields.sequence == 2 && retry.payload.size() == 2 && byte(retry.payload, 0) == 3, "retry contains unacknowledged suffix");
    check(c.snapshot().retransmissions == 0 && c.snapshot().rto_ms == 1000, "rejected retry does not count or back off");
    auto retried = take(c, deadline).parsed(); check(retried.fields.sequence == 2, "retry after rejected submission");
    check(c.snapshot().rto_ms == 2000 && c.snapshot().retransmissions == 1, "accepted retry doubles RTO");
    auto done = peer_packet({0, 0, 0, 4, tcp_ack, 64}); input(c, done, deadline + 1);
    check(!c.snapshot().flight && c.snapshot().send_buffered == 0 && c.snapshot().rto_ms == 2000, "Karn excludes retransmitted RTT");
}
void passive_and_simultaneous()
{
    Connection<> passive;
    check(bool(passive.passive_open(right, 700, 0)), "single-slot passive listener");
    auto spurious = peer_packet({0, 0, 9, 33, tcp_ack, 4096}, {}, {}, left, right); input(passive, spurious, 1);
    auto reset = take(passive, 1).parsed(); check(reset.fields.sequence == 33 && reset.fields.flags == tcp_rst, "listen ACK reset");
    auto syn = peer_packet({0, 0, 900, 0, tcp_syn, 1234}, {}, {}, left, right); input(passive, syn, 2);
    check(passive.snapshot().state == State::syn_received && passive.peer() == left, "passive SYN binds exact peer");
    auto synack = take(passive, 2).parsed(); check(synack.fields.flags == (tcp_syn | tcp_ack) && synack.fields.sequence == 700 && synack.fields.acknowledgment == 901, "passive SYN ACK");
    auto wrong = peer_packet({0, 0, 901, 701, tcp_ack, 1234}, {}, {}, Endpoint{{10, 0, 0, 9}, left.port}, right);
    input(passive, wrong, 3); check(passive.snapshot().state == State::syn_received, "foreign tuple cannot complete handshake");
    auto invalid_syn_ack = peer_packet({0, 0, 900, 701, tcp_syn | tcp_ack, 1234}, {}, {}, left, right);
    input(passive, invalid_syn_ack, 3); check(passive.snapshot().state == State::syn_received, "passive SYN ACK cannot take simultaneous-open path"); take(passive, 3);
    const std::array<std::byte, 1> unexpected_payload{std::byte{42}};
    auto payload_syn_ack = peer_packet({0, 0, 900, 701, tcp_syn | tcp_ack, 1234}, unexpected_payload, {}, left, right);
    input(passive, payload_syn_ack, 3); check(passive.snapshot().state == State::syn_received &&
        passive.snapshot().receive_buffered == 0, "SYN payload cannot bypass passive simultaneous-open exclusion");
    take(passive, 3);
    auto final = peer_packet({0, 0, 901, 701, tcp_ack, 1234}, {}, {}, left, right); input(passive, final, 4);
    check(passive.snapshot().state == State::established, "passive accepts completed handshake only");
    Connection<> server, client;
    check(bool(server.passive_open(right, 700, 0)) && bool(client.active_open(left, right, 900, 0)), "lost SYN-ACK setup");
    auto initial_syn = take(client, 0); input(server, initial_syn, 0); take(server, 0);
    // The first SYN-ACK is deliberately lost; peer retries SYN at its RTO.
    auto duplicate_syn = take(client, 1000); input(server, duplicate_syn, 1000);
    auto retransmitted_syn_ack = take(server, 1000);
    check(retransmitted_syn_ack.parsed().fields.flags == (tcp_syn | tcp_ack) &&
        server.snapshot().send_next == 701, "lost SYN-ACK duplicate SYN retains SYN sequence");
    input(client, retransmitted_syn_ack, 1001); auto completed = take(client, 1001); input(server, completed, 1002);
    check(client.snapshot().state == State::established && server.snapshot().state == State::established, "lost SYN-ACK recovery establishes both peers");
    check(server.snapshot().retransmissions == 0 && !server.snapshot().flight, "recovered passive SYN ownership drains");
    Connection<> retry_listener;
    check(bool(retry_listener.passive_open(right, 10, 0)), "passive retry setup");
    input(retry_listener, initial_syn, 0); take(retry_listener, 0);
    auto cancel = peer_packet({0, 0, 901, 0, tcp_rst, 0}, {}, {}, left, right); input(retry_listener, cancel, 1);
    check(retry_listener.snapshot().state == State::listen && !retry_listener.snapshot().listener_armed, "reset passive incarnation requires fresh ISS");
    check(!retry_listener.receive(initial_syn.view(), 2) && !retry_listener.rearm_listener(10, 2), "stale passive ISS cannot be reused");
    check(bool(retry_listener.rearm_listener(11, 2)), "executor supplies fresh passive ISS"); input(retry_listener, initial_syn, 2);
    check(take(retry_listener, 2).parsed().fields.sequence == 11, "fresh passive incarnation uses replacement ISS");
    Connection<> a, b;
    check(bool(a.active_open(left, right, 100, 0)) && bool(b.active_open(right, left, 200, 0)), "simultaneous active opens");
    auto as = take(a, 0), bs = take(b, 0); input(a, bs, 10); input(b, as, 10);
    check(a.snapshot().state == State::syn_received && b.snapshot().state == State::syn_received, "simultaneous SYN exchange");
    auto aa = take(a, 10), ba = take(b, 10); input(a, ba, 20); input(b, aa, 20);
    check(a.snapshot().state == State::established && b.snapshot().state == State::established, "simultaneous SYN ACK exchange");
    auto af = take(a, 20), bf = take(b, 20); input(a, bf, 30); input(b, af, 30); none(a, 30); none(b, 30);
    Connection<> zero;
    check(bool(zero.active_open(left, right, 100, 0)), "simultaneous zero-window setup"); take(zero, 0);
    auto peer_syn = peer_packet({0, 0, 200, 0, tcp_syn, 32}); input(zero, peer_syn, 900); take(zero, 900);
    check(zero.snapshot().retransmit_deadline == 1900, "simultaneous SYN ACK restarts minimum retry interval");
    auto zero_sa = peer_packet({0, 0, 200, 101, tcp_syn | tcp_ack, 0}); input(zero, zero_sa, 901);
    check(zero.snapshot().state == State::established && zero.snapshot().peer_window == 0, "simultaneous completion refreshes peer flow-control window"); take(zero, 901);
    check(bool(zero.write(unexpected_payload)), "zero peer window still admits bounded stream queue"); none(zero, 902);
    auto reopened = peer_packet({0, 0, 201, 101, tcp_ack, 1}); input(zero, reopened, 903);
    check(take(zero, 903).parsed().payload.size() == 1, "window reopening permits queued stream byte");
}
void receive_flow_and_close()
{
    Connection<32, 8, 8> c; establish(c, 100, 0xfffffffcu);
    std::array<std::byte, 6> text{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
    auto out_of_order = peer_packet({0, 0, 0xffffffffu, 101, tcp_ack, 32}, text); input(c, out_of_order, 101);
    check(c.snapshot().receive_buffered == 0, "out-of-order payload not fabricated");
    check(take(c, 101).parsed().fields.acknowledgment == 0xfffffffdu, "out-of-order cumulative ACK");
    auto first = peer_packet({0, 0, 0xfffffffdu, 101, tcp_ack, 32}, text); input(c, first, 102); first.bytes.fill(std::byte{0});
    check(c.snapshot().receive_next == 3 && c.snapshot().receive_buffered == 6, "receive sequence wraps and copies payload");
    take(c, 102);
    auto overlap = peer_packet({0, 0, 1, 101, tcp_ack, 32}, Bytes{text.data(), 4}); input(c, overlap, 103);
    check(c.snapshot().receive_buffered == 8 && c.snapshot().receive_next == 5, "overlap trimmed to free receive capacity");
    auto zero = take(c, 103).parsed(); check(zero.fields.window == 0 && zero.fields.acknowledgment == 5, "bounded zero window");
    auto cannot = peer_packet({0, 0, 5, 101, tcp_ack | tcp_fin, 32}); input(c, cannot, 104);
    check(!c.snapshot().eof, "zero receive window cannot consume FIN"); take(c, 104);
    std::array<std::byte, 8> read{}; auto n = c.read(read); check(bool(n) && n.value() == 8, "application drains copied receive ring");
    check(read[0] == std::byte{'a'} && read[6] == std::byte{'c'}, "overlap delivers each new byte exactly once");
    check(take(c, 105).parsed().fields.window == 8, "read announces receive window reopening");
    auto fin = peer_packet({0, 0, 5, 101, tcp_ack | tcp_fin, 32}); input(c, fin, 106);
    check(c.snapshot().state == State::close_wait && c.snapshot().eof, "peer FIN half closes read side");
    auto eof = c.read(read); check(bool(eof) && eof.value() == 0, "stream EOF after drain"); take(c, 106);
    auto w = c.write(text); check(bool(w) && w.value() == 6, "write remains usable after peer FIN");
    auto half_packet = take(c, 107); auto tx = half_packet.parsed(); check(tx.payload.size() == 6, "half-close response bytes");
    check(bool(c.shutdown_write()) && !c.write(text), "local shutdown rejects later writes");
    auto ack = peer_packet({0, 0, 6, 107, tcp_ack, 32}); input(c, ack, 108);
    auto closing = take(c, 108).parsed(); check(closing.fields.sequence == 107 && closing.fields.flags == (tcp_fin | tcp_ack), "queued data ACK precedes local FIN");
    check(c.snapshot().state == State::last_ack, "passive close last ACK");
    auto last = peer_packet({0, 0, 6, 108, tcp_ack, 32}); input(c, last, 109);
    check(c.snapshot().state == State::closed && !c.snapshot().flight, "last ACK reclaims retransmission flight"); none(c, 110);
}
void active_close_and_time_wait()
{
    Connection<> c; establish(c);
    check(bool(c.shutdown_write()), "active close request"); take(c, 101); check(c.snapshot().state == State::fin_wait_1, "active FIN sent");
    auto ack = peer_packet({0, 0, 2001, 1002, tcp_ack, 60000}); input(c, ack, 102);
    check(c.snapshot().state == State::fin_wait_2, "own FIN acknowledgment leaves read direction open");
    std::array<std::byte, 1> ch{std::byte{'x'}};
    auto pf = peer_packet({0, 0, 2001, 1002, tcp_ack | tcp_fin, 60000}, ch); input(c, pf, 103);
    check(c.snapshot().state == State::time_wait && c.snapshot().receive_buffered == 1, "peer data FIN enters TIME_WAIT");
    check(take(c, 103).parsed().fields.acknowledgment == 2003, "FIN consumes sequence after data");
    std::array<std::byte, 2> buf{}; auto n = c.read(buf); check(bool(n) && n.value() == 1 && buf[0] == ch[0], "TIME_WAIT retains unread data"); take(c, 104);
    const auto previous_deadline = c.snapshot().time_wait_deadline;
    auto forged_fin = peer_packet({0, 0, 2001, 1003, tcp_ack | tcp_fin, 60000}, ch); input(c, forged_fin, 999); take(c, 999);
    check(c.snapshot().time_wait_deadline == previous_deadline, "future ACK cannot extend TIME_WAIT through duplicate FIN path");
    input(c, pf, 1000); take(c, 1000); auto until = c.snapshot().time_wait_deadline;
    check(until == 1000 + 2 * Connection<>::msl_ms, "duplicate FIN restarts two MSL timer");
    check(bool(c.tick(until - 1)) && c.snapshot().state == State::time_wait, "TIME_WAIT cannot expire early");
    check(bool(c.tick(until)) && c.snapshot().state == State::closed, "TIME_WAIT expires after deadline"); none(c, until);
    Connection<> simultaneous; establish(simultaneous); check(bool(simultaneous.shutdown_write()), "simultaneous close local"); take(simultaneous, 101);
    auto pf2 = peer_packet({0, 0, 2001, 1001, tcp_ack | tcp_fin, 60000}); input(simultaneous, pf2, 102);
    check(simultaneous.snapshot().state == State::closing, "crossing FIN enters CLOSING"); take(simultaneous, 102);
    auto af = peer_packet({0, 0, 2002, 1002, tcp_ack, 60000}); input(simultaneous, af, 103);
    check(simultaneous.snapshot().state == State::time_wait, "crossing FIN acknowledgment enters TIME_WAIT");
}
void time_wait_reset_preserves_unread_bytes()
{
    Connection<> c; establish(c);
    check(bool(c.shutdown_write()), "local half close before peer response"); take(c, 101);
    auto ack = peer_packet({0, 0, 2001, 1002, tcp_ack, 60000}); input(c, ack, 102);
    const std::array<std::byte, 8> english{std::byte{'R'}, std::byte{'E'}, std::byte{'S'}, std::byte{'P'},
        std::byte{'O'}, std::byte{'N'}, std::byte{'S'}, std::byte{'E'}};
    auto response = peer_packet({0, 0, 2001, 1002, tcp_ack | tcp_fin, 60000}, english);
    input(c, response, 103); take(c, 103);
    check(c.snapshot().state == State::time_wait && c.snapshot().receive_buffered == 8,
          "completed close retains full unread response");
    const auto until = c.snapshot().time_wait_deadline;
    std::array<std::byte, 3> prefix{}; auto first = c.read(prefix);
    check(bool(first) && first.value() == 3 && prefix[0] == english[0] && prefix[2] == english[2],
          "partial response read before closed peer reset"); take(c, 104);
    for (u32 sequence : {2010u, 2011u, 2009u})
    {
        auto reset = peer_packet({0, 0, sequence, 0, tcp_rst, 0});
        auto received = c.receive(reset.view(), 105);
        check(bool(received) && received.value() == Ingress::ignored, "TIME_WAIT ignores peer resets");
        check(c.snapshot().state == State::time_wait && c.snapshot().failure == Failure::none &&
              c.snapshot().receive_buffered == 5 && c.snapshot().time_wait_deadline == until,
              "reset preserves unread suffix and original TIME_WAIT deadline"); none(c, 105);
    }
    std::array<std::byte, 8> suffix{}; auto remaining = c.read(suffix);
    check(bool(remaining) && remaining.value() == 5, "remaining response survives exact-sequence reset");
    for (usize i = 0; i < 5; ++i) check(suffix[i] == english[i + 3], "remaining response bytes retain order");
    take(c, 106); auto eof = c.read(suffix);
    check(bool(eof) && eof.value() == 0, "EOF remains readable after ignored TIME_WAIT reset");
    check(bool(c.tick(until - 1)) && c.snapshot().state == State::time_wait, "reset cannot shorten TIME_WAIT");
    check(bool(c.tick(until)) && c.snapshot().state == State::closed, "ignored reset cannot extend TIME_WAIT");
    none(c, until);
}
void adversarial_and_rto()
{
    Connection<> c; establish(c, 1000, 2000, 0, 1000); check(c.snapshot().rto_ms == 3000, "first RTT initializes SRTT and variance");
    std::array<std::byte, 1> x{std::byte{1}}; check(bool(c.write(x)), "RTT sample data"); take(c, 1001);
    auto ack = peer_packet({0, 0, 2001, 1002, tcp_ack, 60000}); input(c, ack, 3001);
    check(c.snapshot().rto_ms == 3625, "subsequent RTT variance uses old SRTT");
    auto rst = peer_packet({0, 0, 2002, 0, tcp_rst, 0}); input(c, rst, 3002);
    check(c.snapshot().state == State::established, "in-window nonexact reset requires challenge"); take(c, 3002);
    input(c, rst, 3003); none(c, 3003);
    auto outside = peer_packet({0, 0, 9000, 0, tcp_rst, 0}); input(c, outside, 3004); none(c, 3004);
    auto syn = peer_packet({0, 0, 2001, 0, tcp_syn, 60000}); input(c, syn, 4002); take(c, 4002);
    check(c.snapshot().state == State::established, "unexpected SYN cannot replace connection");
    auto urgent = peer_packet({0, 0, 2001, 1002, tcp_ack | tcp_urg, 60000}); check(!c.receive(urgent.view(), 4003), "unsupported urgent data is explicit");
    auto damaged = ack; damaged.bytes[36] ^= std::byte{1}; check(!c.receive(damaged.view(), 4004), "bad checksum cannot change transport state");
    auto exact = peer_packet({0, 0, 2001, 0, tcp_rst, 0}); input(c, exact, 4005);
    check(c.snapshot().state == State::closed && c.snapshot().failure == Failure::reset, "exact reset destroys stream queues");
    Connection<> timeout; check(bool(timeout.active_open(left, right, 1, 0)), "timeout connection"); take(timeout, 0);
    for (u8 retry = 0; retry < Connection<>::max_retransmissions; ++retry)
    {
        const auto until = timeout.snapshot().retransmit_deadline; none(timeout, until - 1); take(timeout, until);
        check(timeout.snapshot().retransmissions == retry + 1 && timeout.snapshot().rto_ms <= 60000, "bounded retry and maximum RTO");
    }
    const auto until = timeout.snapshot().retransmit_deadline; check(bool(timeout.tick(until)), "retry exhaustion timer");
    check(timeout.snapshot().state == State::closed && timeout.snapshot().failure == Failure::timed_out && !timeout.snapshot().flight, "retry exhaustion fails instead of inventing connection");
    Connection<> lost_syn; check(bool(lost_syn.active_open(left, right, 1000, 0)), "lost SYN open"); take(lost_syn, 0); take(lost_syn, 1000);
    auto sa = peer_packet({0, 0, 2000, 1001, tcp_syn | tcp_ack, 60000}); input(lost_syn, sa, 1001);
    check(lost_syn.snapshot().rto_ms >= 3000, "SYN retransmission resets data RTO to at least three seconds");
}
void invalid_ack_oracle()
{
    Connection<32, 32, 16> c; establish(c, 0xfffffff0u, 100);
    std::array<std::byte, 8> data{}; check(bool(c.write(data)), "invalid ACK oracle queued bytes"); take(c, 101);
    const u32 una = c.snapshot().send_unacknowledged, next = c.snapshot().send_next;
    u32 random = 0x92936298u;
    for (u32 i = 0; i < 1024; ++i)
    {
        random = random * 1664525u + 1013904223u;
        const u32 ack = i < 512 ? next + 1 + (random & 0x7fffu) : una - 65536u - (random & 0x7fffu);
        // Independent unsigned finite-interval test, not transport serial helpers.
        check(u32{ack - (una - 65535u)} > u32{next - (una - 65535u)}, "oracle ACK lies outside admitted range");
        auto invalid = peer_packet({0, 0, 101, ack, tcp_ack, 60000}, data); input(c, invalid, 102 + i);
        check(c.snapshot().send_buffered == 8 && c.snapshot().send_unacknowledged == una &&
            c.snapshot().receive_buffered == 0, "invalid ACK cannot release TX or inject RX bytes");
    }
}
void stream_ring_stress()
{
    // Deterministic independent byte oracle across wrap, short writes and ring
    // reuse; peer ACKs are built from observed wire sequence plus payload count.
    for (u32 seed = 0; seed < 40; ++seed)
    {
        Connection<17, 19, 7> c; const u32 iss = 0xfffffff0u + seed;
        establish(c, iss, 500 + seed); u64 now = 101; usize next = 0;
        while (next < 151)
        {
            std::array<std::byte, 25> bytes{};
            const usize wanted = (151 - next) < bytes.size() ? 151 - next : bytes.size();
            for (usize i = 0; i < wanted; ++i) bytes[i] = std::byte{static_cast<u8>(next + i + seed)};
            auto w = c.write(Bytes{bytes.data(), wanted}); check(bool(w), "short ring write succeeds");
            const usize admitted = w.value(); usize delivered = 0;
            while (delivered < admitted)
            {
                auto pkt = take(c, now++); auto p = pkt.parsed();
                check(p.payload.size() <= 7 && !p.payload.empty(), "effective segment capacity");
                for (usize i = 0; i < p.payload.size(); ++i)
                    check(byte(p.payload, i) == static_cast<u8>(next + delivered + i + seed), "independent stream byte oracle");
                delivered += p.payload.size();
                auto a = peer_packet({0, 0, 501 + seed, p.fields.sequence + static_cast<u32>(p.payload.size()), tcp_ack, 60000});
                input(c, a, now++);
            }
            next += admitted; check(c.snapshot().send_buffered == 0, "ring ownership drains after ACK");
        }
        check(c.snapshot().send_next == iss + 1 + 151, "wrapping stream sequence accounts all bytes");
    }
}
}
int main()
{
    serial_tests(); ownership_and_handshake(); passive_and_simultaneous(); receive_flow_and_close();
    active_close_and_time_wait(); time_wait_reset_preserves_unread_bytes(); adversarial_and_rto(); invalid_ack_oracle(); stream_ring_stress();
    std::printf("TCP_TRANSPORT_HOSTED_PASS checks=%zu guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
