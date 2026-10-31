#include <ok/net/tcp_transport.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

using namespace ok;
using namespace ok::net::tcp;
using namespace ok::net::wire;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "TCP_WINDOW_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

constexpr Endpoint left{{10, 0, 0, 2}, 32001}, right{{10, 0, 0, 3}, 8080};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    Bytes view() const { return {bytes.data(), length}; }
    Tcp parsed() const { auto ip = ipv4(view()); CHECK(ip); auto p = tcp(ip.value()); CHECK(p); return p.value(); }
};
Packet peer(TcpFields fields, Bytes payload = {})
{
    Packet p; fields.source_port = right.port; fields.destination_port = left.port;
    auto n = build_tcp(p.bytes, right.address, left.address, 10, 64, fields, {}, payload); CHECK(n);
    p.length = n.value(); return p;
}
template <class C> std::optional<Packet> next(C &c, u64 now)
{
    Packet p; auto n = c.prepare(p.bytes, now); CHECK(n);
    if (!n.value()) return {};
    p.length = n.value()->length; CHECK(c.complete_transmit(n.value()->token, true, now)); return p;
}
template <class C> Packet take(C &c, u64 now) { auto p = next(c, now); CHECK(p); return *p; }
// Parsed views borrow the packet; kept packets outlive every check.
std::deque<Packet> kept;
Tcp seen(const Packet &p) { kept.push_back(p); return kept.back().parsed(); }
template <class C> void input(C &c, const Packet &p, u64 now) { CHECK(c.receive(p.view(), now)); }
using Big = Connection<16384, 4096, 536>;
constexpr u32 iss = 1000, irs = 5000;
// Established with the peer advertising `window` and no MSS option (536).
template <class C> void establish(C &c, u16 window = 60000)
{
    CHECK(c.active_open(left, right, iss, 0)); take(c, 0);
    input(c, peer({0, 0, irs, iss + 1, tcp_syn | tcp_ack, window}), 100);
    CHECK(seen(take(c, 100)).fields.flags == tcp_ack);
}
std::vector<std::byte> pattern(usize size, u32 seed = 1)
{
    std::vector<std::byte> out(size);
    for (auto &b : out) { seed = seed * 1103515245u + 12345u; b = std::byte{static_cast<u8>(seed >> 16)}; }
    return out;
}
Packet ack(u32 value, u16 window = 60000) { return peer({0, 0, irs + 1, value, tcp_ack, window}); }
// Every segment the connection is willing to send now.
template <class C> std::vector<Tcp> burst(C &c, u64 now, std::deque<Packet> &store)
{
    std::vector<Tcp> out;
    while (auto p = next(c, now)) { store.push_back(*p); out.push_back(store.back().parsed()); }
    return out;
}

void slow_start_and_peer_window()
{
    Big c; establish(c);
    const auto data = pattern(16000);
    CHECK(c.write(data).value() == 16000);
    std::deque<Packet> store;
    auto first = burst(c, 101, store);
    // RFC 5681 initial window: min(4 * 536, max(2 * 536, 4380)) = 2144 bytes.
    CHECK(first.size() == 4 && c.snapshot().in_flight == 2144 && c.snapshot().congestion_window == 2144);
    for (usize i = 0; i < first.size(); ++i)
    {
        CHECK(first[i].fields.sequence == iss + 1 + 536 * i && first[i].payload.size() == 536);
        CHECK(first[i].payload[0] == data[536 * i]);
    }
    // Slow start grows by at most one segment per ACK (RFC 5681 section 3.1).
    input(c, ack(iss + 1 + 536), 200);
    CHECK(c.snapshot().congestion_window == 2680 && c.snapshot().in_flight == 1608);
    for (u32 i = 2; i <= 4; ++i) input(c, ack(iss + 1 + 536 * i), 200);
    CHECK(c.snapshot().congestion_window == 4288 && c.snapshot().in_flight == 0 && !c.snapshot().flight);
    auto second = burst(c, 200, store);
    CHECK(second.size() == 8 && c.snapshot().in_flight == 4288);
    // A small peer window bounds the flight below the congestion window.
    Big narrow; establish(narrow, 1000);
    CHECK(narrow.write(data));
    store.clear(); auto limited = burst(narrow, 101, store);
    CHECK(limited.size() == 2 && limited[0].payload.size() == 536 && limited[1].payload.size() == 464);
    CHECK(narrow.snapshot().in_flight == 1000);
    input(narrow, ack(iss + 1 + 536, 1000), 150);
    limited = burst(narrow, 150, store);
    CHECK(limited.size() == 1 && limited[0].payload.size() == 536 && narrow.snapshot().in_flight == 1000);
}

void fast_retransmit_and_recovery()
{
    Big c; establish(c);
    const auto data = pattern(16000, 7); CHECK(c.write(data));
    std::deque<Packet> store; auto flight = burst(c, 101, store);
    for (u32 i = 1; i <= 4; ++i) input(c, ack(iss + 1 + 536 * i), 150);
    flight = burst(c, 150, store);
    CHECK(flight.size() == 8);
    const u32 base = iss + 1 + 2144;
    // Segment base is lost; the peer acknowledges up to it once, then three duplicates.
    for (int i = 0; i < 2; ++i) input(c, ack(base), 200 + i);
    CHECK(c.snapshot().duplicate_acks == 2 && !c.snapshot().recovering && !next(c, 202));
    input(c, ack(base), 203);
    CHECK(c.snapshot().recovering && c.snapshot().duplicate_acks == 3);
    const u32 flight_bytes = 8 * 536;
    CHECK(c.snapshot().slow_start_threshold == flight_bytes / 2 && c.snapshot().congestion_window == flight_bytes / 2 + 3 * 536);
    auto retransmitted = seen(take(c, 203));
    CHECK(retransmitted.fields.sequence == base && retransmitted.payload.size() == 536 && retransmitted.payload[0] == data[2144]);
    CHECK(c.snapshot().retransmissions == 0);
    // Further duplicates inflate the window by one segment each and release new data.
    u32 window = c.snapshot().congestion_window;
    for (int i = 0; i < 4; ++i) { input(c, ack(base), 210 + i); CHECK(c.snapshot().congestion_window == window + 536 * (i + 1)); }
    auto released = burst(c, 214, store);
    CHECK(!released.empty() && released[0].fields.sequence == iss + 1 + 2144 + 8 * 536);
    // A partial ACK (a second hole) retransmits the next missing segment at once.
    input(c, ack(base + 3 * 536), 220);
    CHECK(c.snapshot().recovering);
    auto hole = seen(take(c, 220));
    CHECK(hole.fields.sequence == base + 3 * 536 && hole.payload[0] == data[2144 + 3 * 536]);
    // The full ACK ends recovery with the window at the threshold.
    const u32 high = c.snapshot().send_next;
    input(c, ack(high), 230);
    CHECK(!c.snapshot().recovering && c.snapshot().congestion_window <= flight_bytes / 2 + 536);
    CHECK(c.snapshot().congestion_window >= 536);
}

void timeout_goes_back()
{
    Big c; establish(c);
    const auto data = pattern(8000, 3); CHECK(c.write(data));
    std::deque<Packet> store; auto flight = burst(c, 101, store);
    CHECK(flight.size() == 4);
    const u64 deadline = c.snapshot().retransmit_deadline;
    CHECK(deadline == 101 + 1000 && !next(c, deadline - 1));
    auto retry = seen(take(c, deadline));
    CHECK(retry.fields.sequence == iss + 1 && retry.payload.size() == 536);
    CHECK(c.snapshot().retransmissions == 1 && c.snapshot().rto_ms == 2000);
    CHECK(c.snapshot().congestion_window == 536 && c.snapshot().slow_start_threshold == 1072);
    CHECK(c.snapshot().send_next == iss + 1 + 536 && !next(c, deadline));
    // The ACK of the head restarts the timer, grows the window, and the rest is resent in order.
    input(c, ack(iss + 1 + 536), deadline + 10);
    CHECK(c.snapshot().retransmissions == 0 && c.snapshot().congestion_window == 1072);
    auto resent = burst(c, deadline + 10, store);
    CHECK(resent.size() == 2 && resent[0].fields.sequence == iss + 1 + 536 && resent[1].fields.sequence == iss + 1 + 1072);
    // Karn: retransmitted data gives no RTT sample.
    input(c, ack(iss + 1 + 1608), deadline + 500);
    CHECK(c.snapshot().rto_ms == 2000);
}

void reassembly()
{
    Connection<1024, 2048, 536> c; establish(c);
    const auto data = pattern(1500, 9);
    const auto segment = [&](usize at, usize size, u8 flags = tcp_ack) {
        return peer({0, 0, irs + 1 + static_cast<u32>(at), iss + 1, flags, 60000}, Bytes{data.data() + at, size}); };
    // Third, then second segment arrive first; each elicits an immediate duplicate ACK.
    input(c, segment(1000, 500, tcp_ack | tcp_fin), 200);
    auto dup = seen(take(c, 200));
    CHECK(dup.fields.acknowledgment == irs + 1 && c.snapshot().out_of_order == 500 && c.snapshot().receive_buffered == 0);
    input(c, segment(500, 500), 201); take(c, 201);
    CHECK(c.snapshot().out_of_order == 1000 && c.snapshot().state == State::established);
    // A retransmitted overlap inside a kept range changes nothing.
    input(c, segment(600, 100), 202); take(c, 202);
    CHECK(c.snapshot().out_of_order == 1000);
    // The first segment fills the gap: all bytes and the kept FIN take effect.
    input(c, segment(0, 500), 203);
    auto all = seen(take(c, 203));
    CHECK(all.fields.acknowledgment == irs + 1 + 1500 + 1 && c.snapshot().receive_buffered == 1500);
    CHECK(c.snapshot().out_of_order == 0 && c.snapshot().eof && c.snapshot().state == State::close_wait);
    std::vector<std::byte> read(1500);
    CHECK(c.read(read).value() == 1500 && read == data);

    // More disjoint ranges than records: the excess is dropped, not corrupted.
    Connection<1024, 4096, 536> d; establish(d);
    const auto many = pattern(4000, 11);
    const auto piece = [&](usize at) { return peer({0, 0, irs + 1 + static_cast<u32>(at), iss + 1, tcp_ack, 60000},
                                                   Bytes{many.data() + at, 100}); };
    for (usize i = 1; i <= 6; ++i) { input(d, piece(i * 200), 300); take(d, 300); }
    CHECK(d.snapshot().out_of_order == 400 && d.snapshot().receive_buffered == 0);
    input(d, peer({0, 0, irs + 1, iss + 1, tcp_ack, 60000}, Bytes{many.data(), 1300}), 301); take(d, 301);
    CHECK(d.snapshot().receive_next == irs + 1 + 1300 && d.snapshot().receive_buffered == 1300 && d.snapshot().out_of_order == 0);
    std::vector<std::byte> got(1300);
    CHECK(d.read(got).value() == 1300 && std::equal(got.begin(), got.end(), many.begin()));
}

void zero_window_probe()
{
    Big c; establish(c);
    const auto data = pattern(3000, 5); CHECK(c.write(data));
    std::deque<Packet> store; burst(c, 101, store);
    // The peer takes everything but closes its window.
    for (u32 i = 1; i <= 3; ++i) input(c, ack(iss + 1 + 536 * i), 150);
    input(c, ack(iss + 1 + 2144, 0), 150);
    CHECK(!c.snapshot().flight && !next(c, 150) && !next(c, 1149));
    auto probe = seen(take(c, 1150));
    CHECK(probe.payload.size() == 1 && probe.fields.sequence == iss + 1 + 2144 && probe.payload[0] == data[2144]);
    // An unanswered probe is retransmitted with backoff.
    auto again = seen(take(c, 2150));
    CHECK(again.fields.sequence == iss + 1 + 2144 && c.snapshot().rto_ms == 2000);
    // The window reopens with the probe byte acknowledged.
    input(c, ack(iss + 1 + 2145, 4096), 2200);
    auto rest = burst(c, 2200, store);
    usize sent = 0; for (const auto &s : rest) sent += s.payload.size();
    CHECK(sent == 3000 - 2145 && rest[0].fields.sequence == iss + 1 + 2145);
}

// Two endpoints over a channel that drops and reorders: the whole stream and
// both FINs arrive intact.
void lossy_transfer(u32 seed, u32 drop_percent)
{
    Connection<8192, 4096, 536> a, b;
    CHECK(b.passive_open(right, 7000, 0) && a.active_open(left, right, 100, 0));
    struct Flight { u64 at; bool to_b; Packet packet; };
    std::deque<Flight> wire;
    u32 random = seed;
    const auto chance = [&](u32 percent) { random = random * 1664525u + 1013904223u; return (random >> 8) % 100 < percent; };
    const auto stream = pattern(30000, seed);
    usize written = 0; std::vector<std::byte> received; bool closed = false;
    u64 now = 0;
    for (u32 round = 0; round < 200000 && !(b.snapshot().state == State::close_wait && received.size() == stream.size() &&
         a.snapshot().state == State::fin_wait_2); ++round)
    {
        if (a.snapshot().state == State::established && written < stream.size())
        {
            auto w = a.write(Bytes{stream.data() + written, stream.size() - written});
            if (w) written += w.value();
        }
        if (written == stream.size() && !closed && a.snapshot().state == State::established) { CHECK(a.shutdown_write()); closed = true; }
        for (auto *side : {&a, &b})
            while (auto p = next(*side, now))
            {
                if (chance(drop_percent)) continue;
                // Reordering: some packets take 5 ms longer.
                wire.push_back({now + (chance(20) ? 5u : 1u), side == &a, *p});
            }
        bool moved = false;
        for (usize i = 0; i < wire.size();)
        {
            if (wire[i].at > now) { ++i; continue; }
            auto f = wire[i]; wire.erase(wire.begin() + static_cast<long>(i));
            auto &target = f.to_b ? b : a;
            static_cast<void>(target.receive(f.packet.view(), now));
            moved = true;
        }
        std::array<std::byte, 700> buffer{};
        for (;;)
        {
            auto r = b.read(buffer);
            if (!r || !r.value()) break;
            received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<long>(r.value()));
        }
        if (!moved && wire.empty())
        {
            // Idle: jump to the next timer.
            u64 wake = now + 50;
            for (auto *side : {&a, &b}) if (side->snapshot().flight) wake = std::min(wake, side->snapshot().retransmit_deadline);
            now = wake > now ? wake : now + 1;
        }
        else ++now;
        CHECK(a.snapshot().failure == Failure::none && b.snapshot().failure == Failure::none);
    }
    CHECK(received == stream);
    CHECK(b.snapshot().eof && b.snapshot().state == State::close_wait && a.snapshot().state == State::fin_wait_2);
}
}

int main()
{
    slow_start_and_peer_window();
    fast_retransmit_and_recovery();
    timeout_goes_back();
    reassembly();
    zero_window_probe();
    lossy_transfer(1, 0);
    for (u32 seed = 2; seed < 12; ++seed) lossy_transfer(seed, 10);
    lossy_transfer(99, 25);
    std::printf("PASS tcp window scope=model checks=%zu\n", checks);
}
