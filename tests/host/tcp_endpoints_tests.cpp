#include <ok/net/tcp_executor.hpp>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using namespace ok;
using namespace ok::net::tcp;
using namespace ok::net::wire;
namespace
{
usize checks{};
void check(bool passed, const char *why) { ++checks; if (!passed) { std::fprintf(stderr, "FAIL check=%zu %s\n", checks, why); std::exit(1); } }
constexpr Endpoint local{{10, 0, 0, 2}, 32001}, peer{{10, 0, 0, 3}, 8080};
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    Bytes view() const { return {bytes.data(), length}; }
    Tcp parsed() const { auto ip = ipv4(view()); check(bool(ip), "independent packet IPv4 checksum"); auto p = tcp(ip.value()); check(bool(p), "independent packet TCP checksum"); return p.value(); }
};
Packet packet(Endpoint from, Endpoint to, u32 sequence, u32 acknowledgment, u8 flags, u16 window = 4096, Bytes payload = {})
{
    Packet p; auto result = build_tcp(p.bytes, from.address, to.address, 1, 64,
        {from.port, to.port, sequence, acknowledgment, flags, window}, {}, payload);
    check(bool(result), "independent peer packet build"); p.length = result.value(); return p;
}
struct Iss { u32 next{500}; usize calls{}; Result<u32> operator()(Endpoint, Endpoint) { ++calls; return next++; } };
template <typename T> void ingress(T &table, const Packet &p, u64 now, Iss &iss)
{ auto result = table.ingest(p.view(), now, iss); check(bool(result), "endpoint valid ingress"); }
template <typename T> Packet take(T &table, u64 now, bool accepted = true)
{
    Packet p; auto out = table.prepare(p.bytes, now); check(bool(out) && bool(out.value()), "endpoint expected packet");
    p.length = out.value()->length; check(bool(table.complete(out.value()->ticket, accepted, now)), "endpoint submission completion"); return p;
}
template <typename T> void none(T &table, u64 now)
{ Packet p; auto out = table.prepare(p.bytes, now); check(bool(out) && !out.value(), "endpoint no unexpected packet"); }
template <typename T> Handle establish(T &table, Iss &iss, u32 own = 100, u32 other = 200, u64 start = 0, u16 window = 4096)
{
    auto opened = table.connect(local, peer, own, start); check(bool(opened), "table active connection"); auto h = opened.value();
    auto syn = take(table, start); check(syn.parsed().fields.sequence == own, "table emits initial SYN");
    auto sa = packet(peer, local, other, own + 1, tcp_syn | tcp_ack, window); ingress(table, sa, start + 1, iss); take(table, start + 1);
    check(table.state(h).value().transport.state == State::established, "table active handshake established"); return h;
}
using Small = EndpointTable<4, 2, 2, 32, 32, 16>;
void handle_and_packet_lifecycle()
{
    static_assert(!std::is_copy_constructible_v<Small> && !std::is_move_constructible_v<Small>);
    Small table{{1}}, other{{2}}; Iss iss;
    auto opened = table.connect(local, peer, 100, 0); check(bool(opened), "opened generation-tagged connection"); auto h = opened.value();
    check(!other.state(h) && !table.state(Handle{h.owner, h.generation + 1, h.slot, h.kind}), "owner/generation handle refusal");
    check(!table.state(Handle{h.owner, h.generation, h.slot, HandleKind::listener}), "typed handle refusal");
    check(!table.connect(local, peer, 101, 0), "four-tuple collision refused");
    check(bool(table.retain(h)), "canonical owner retained"); check(bool(table.release(h, 100)), "one retained owner released");
    check(table.pools().monotonic_ms == 100 && !table.tick(50), "successful retained release advances table clock");
    Packet p; auto prepared = table.prepare(p.bytes, 100); check(bool(prepared) && bool(prepared.value()), "table packet prepared"); auto ticket = prepared.value()->ticket;
    auto wrong = ticket; ++wrong.owner; check(!table.complete(wrong, true, 100), "cross-owner completion refused");
    wrong = ticket; ++wrong.serial; check(!table.complete(wrong, true, 100), "wrong serial completion refused");
    check(!table.complete(ticket, true, 99) && table.pools().prepared, "backward completion preserves outstanding token");
    check(bool(table.release(h, 100)), "last release while TX prepared");
    check(!table.state(h) && table.pools().connections == 1 && table.pools().retiring == 1, "closed handle revoked but selected slot pinned");
    check(!table.connect(local, peer, 101, 100), "pending retired tuple cannot be reused");
    check(bool(table.complete(ticket, false, 100)), "rejected completion drains revoked handshaking endpoint");
    check(table.pools().connections == 0 && !table.pools().prepared && !table.complete(ticket, true, 100), "retired slot drains and ticket replay refused");
    auto reused = table.connect(local, peer, 101, 100); check(bool(reused) && reused.value().generation > h.generation, "slot reuse increments generation");
    check(!table.write(h, Bytes{}) && !table.readiness(h) && !table.retain(h) && !table.release(h, 100), "all stale application operations refused");
    auto pending = table.prepare(p.bytes, 101); check(bool(pending) && bool(pending.value()), "reuse prepared packet");
    auto old_ticket = pending.value()->ticket; check(bool(table.retire()), "explicit retirement drains prepared token at known valid time");
    check(!table.pools().prepared && !table.complete(old_ticket, true, 101) && table.readiness(reused.value()).value().terminal, "retirement wakes terminal owner and refuses old callback");
    check(bool(table.release(reused.value(), 101)) && table.pools().connections == 0, "terminal application reference can be released");
    static_cast<void>(iss);
}
void passive_queues()
{
    Small table{{3}}; Iss iss;
    auto listening = table.listen(peer, 2); check(bool(listening), "bounded listener creation"); auto l = listening.value();
    check(!table.listen(peer, 1), "duplicate listener endpoint refused");
    const Endpoint c1 = local, c2{local.address, 32002}, c3{local.address, 32003};
    ingress(table, packet(c1, peer, 10, 0, tcp_syn), 0, iss);
    ingress(table, packet(c2, peer, 20, 0, tcp_syn), 0, iss);
    auto full = table.ingest(packet(c3, peer, 30, 0, tcp_syn).view(), 0, iss);
    check(bool(full) && full.value().dropped && iss.calls == 2 && table.pools().unaccepted == 2, "backlog caps pending/completed children and ISS calls");
    check(!table.accept(l) && table.readiness(l).value().accept_count == 0, "SYN-RECEIVED cannot be accepted");
    auto first = take(table, 0), second = take(table, 0);
    const auto a = first.parsed().fields, b = second.parsed().fields;
    check(a.destination_port == c1.port && b.destination_port == c2.port, "fair passive egress order");
    ingress(table, packet(c2, peer, 21, b.sequence + 1, tcp_ack), 1, iss);
    ingress(table, packet(c1, peer, 11, a.sequence + 1, tcp_ack), 1, iss);
    auto ready = table.readiness(l); check(bool(ready) && ready.value().accept_count == 2 && !ready.value().may_wait(l, ready.value().sequence), "completed queue readiness");
    auto h2 = table.accept(l), h1 = table.accept(l); check(bool(h2) && bool(h1), "completed child ownership transfer");
    check(table.state(h2.value()).value().peer == c2 && table.state(h1.value()).value().peer == c1, "accept FIFO follows completion order");
    check(table.pools().unaccepted == 0 && !table.accept(l), "accept removes exactly once");
    ingress(table, packet(c3, peer, 30, 0, tcp_syn), 2, iss); take(table, 2);
    ingress(table, packet(c3, peer, 31, 0, tcp_rst), 3, iss);
    check(table.pools().unaccepted == 0, "unaccepted reset consumes incarnation instead of rearming core LISTEN");
    check(bool(table.release(l, 4)), "listener closed while accepted owners remain");
    check(bool(table.state(h1.value())) && bool(table.state(h2.value())), "accepted endpoints outlive listener independently");
    check(bool(table.abort(h1.value(), 4)) && bool(table.release(h1.value(), 4)), "accepted owner abort/release");
    check(bool(table.abort(h2.value(), 4)) && bool(table.release(h2.value(), 4)) && table.pools().connections == 0, "accepted pools drain");
}
void listener_close_selected_child()
{
    Small table{{4}}; Iss iss; auto l = table.listen(peer, 2).value();
    ingress(table, packet(local, peer, 1, 0, tcp_syn), 0, iss);
    ingress(table, packet(Endpoint{local.address, 32002}, peer, 2, 0, tcp_syn), 0, iss);
    Packet p; auto prepared = table.prepare(p.bytes, 0); check(bool(prepared) && bool(prepared.value()), "passive selected child token");
    auto ticket = prepared.value()->ticket; check(bool(table.release(l, 0)), "listener release with child token");
    check(!table.readiness(l) && table.pools().listeners == 0 && table.pools().connections == 1 && table.pools().retiring == 1, "listener revoked, other child freed, selected child pinned");
    check(bool(table.complete(ticket, true, 0)) && table.pools().connections == 0, "accepted stale-listener SYN ACK completes then aborts pinned child");
    check(!table.complete(ticket, false, 0), "child token cannot release twice");
}
void streams_readiness_and_retirement()
{
    Small table{{5}}; Iss iss; auto h = establish(table, iss);
    auto before = table.readiness(h).value();
    check(before.may_wait(h, before.sequence, Readiness::read_interest) && !before.may_wait(h, before.sequence, Readiness::write_interest), "interest-specific connected read wait and send capacity");
    std::array<std::byte, 5> bytes{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
    auto written = table.write(h, bytes); check(bool(written) && written.value() == 5, "endpoint copies application write"); bytes.fill(std::byte{'z'});
    auto tx = take(table, 2); check(byte(tx.parsed().payload, 0) == 'h', "caller mutation cannot change queued stream");
    check(table.state(h).value().transport.send_buffered == 5, "device completion does not acknowledge TCP payload");
    auto answer = packet(peer, local, 201, 106, tcp_ack | tcp_fin, 4096, Bytes{tx.parsed().payload});
    ingress(table, answer, 3, iss); answer.bytes.fill(std::byte{});
    auto ready = table.readiness(h).value(); check(ready.readable_bytes == 5 && ready.eof && ready.sequence != before.sequence, "copied receive/EOF readiness generation");
    std::array<std::byte, 8> output{}; auto read = table.read(h, output); check(bool(read) && read.value() == 5 && output[0] == std::byte{'h'}, "caller-independent receive copy");
    check(table.read(h, output).value() == 0, "endpoint EOF after copied drain"); take(table, 3);
    check(bool(table.release(h, 4)) && !table.state(h), "last canonical owner revoked during half close");
    auto fin = take(table, 4).parsed(); check(fin.fields.flags == (tcp_fin | tcp_ack), "released endpoint retains FIN protocol work");
    ingress(table, packet(peer, local, 207, fin.fields.sequence + 1, tcp_ack), 5, iss);
    check(table.pools().connections == 0, "FIN acknowledgment frees unowned LAST-ACK endpoint");
    Small zero{{6}}; auto z = establish(zero, iss, 1, 2, 0, 0);
    check(bool(zero.release(z, 2)) && zero.pools().connections == 1, "zero peer window retains graceful retirement"); none(zero, 2);
    check(bool(zero.tick(2 + Small::retirement_ms - 1)) && zero.pools().connections == 1, "retirement bound not early");
    check(bool(zero.tick(2 + Small::retirement_ms)) && zero.pools().connections == 0, "zero-window retirement bound prevents tuple leak");
}
void selected_data_close_and_time_wait()
{
    Small table{{16}}; Iss iss; auto h = establish(table, iss);
    const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    check(table.write(h, bytes).value() == 3, "selected close data enqueue");
    Packet output; auto prepared = table.prepare(output.bytes, 2);
    check(bool(prepared) && bool(prepared.value()), "selected close data packet prepared");
    const auto ticket = prepared.value()->ticket;
    check(bool(table.release(h, 2)) && !table.write(h, bytes) && table.pools().connections == 1, "prepared stream release revokes caller and pins protocol bytes");
    check(bool(table.complete(ticket, true, 2)) && table.pools().connections == 1, "accepted selected stream retains unacknowledged protocol ownership");
    ingress(table, packet(peer, local, 201, 104, tcp_ack), 3, iss);
    auto fin = take(table, 3); check(fin.parsed().fields.sequence == 104, "unowned data ACK precedes FIN sequence");
    ingress(table, packet(peer, local, 201, 105, tcp_ack | tcp_fin), 6, iss); take(table, 6);
    check(bool(table.tick(2 + Small::retirement_ms)) && table.pools().connections == 1, "graceful retirement deadline cannot shorten TIME_WAIT");
    check(bool(table.tick(6 + 2 * Connection<>::msl_ms)) && table.pools().connections == 0, "unowned TIME_WAIT retains full two MSL then drains");
    auto fresh = table.connect(local, peer, 300, 6 + 2 * Connection<>::msl_ms);
    check(bool(fresh) && fresh.value().generation > h.generation, "closed tuple reuse begins fresh handle incarnation");
}
void quotas_aliases_and_callback()
{
    EndpointTable<1, 1, 1, 8, 8, 4> generation{{7}, {2, 100, 100, 2}};
    for (u32 i = 0; i < 2; ++i)
    { auto h = generation.connect(local, peer, i, i).value(); check(bool(generation.release(h, i)), "limited generation release"); }
    check(!generation.connect(local, peer, 3, 3), "generation does not wrap after slot exhaustion");
    Small references{{8}, {100, 100, 100, 2}}; auto h = references.connect(local, peer, 1, 0).value();
    check(bool(references.retain(h)) && !references.retain(h), "bounded canonical references");
    Small alias{{9}}; Iss iss; auto a = establish(alias, iss);
    auto self = Bytes{reinterpret_cast<const std::byte *>(&alias), sizeof(alias)};
    auto mutable_self = std::span<std::byte>{reinterpret_cast<std::byte *>(&alias), sizeof(alias)};
    check(!alias.write(a, self) && !alias.read(a, mutable_self) && !alias.ingest(self, 1, iss) && !alias.prepare(mutable_self, 1), "full owner table alias exclusion");
    Small sequence{{10}, {100, 2, 100, 100}}; auto s = sequence.connect(local, peer, 1, 0).value();
    Packet p; auto selected = sequence.prepare(p.bytes, 0); check(bool(selected) && bool(selected.value()), "small readiness packet preparation");
    check(bool(sequence.complete(selected.value()->ticket, false, 0)), "readiness saturation drains token");
    auto terminal = sequence.readiness(s).value(); check(terminal.terminal && !terminal.may_wait(s, terminal.sequence), "readiness saturation cannot equal a waitable epoch");
    Small serial{{11}, {100, 100, 1, 100}}; auto sh = serial.connect(local, peer, 1, 0).value(); take(serial, 0, false);
    check(!serial.prepare(p.bytes, 0) && serial.pools().retired && serial.readiness(sh).value().terminal, "TX ticket serial exhaustion is terminal instead of replaying");
    Small callback{{12}}; auto listener = callback.listen(peer, 1).value();
    struct Reentrant
    {
        Small &table; usize calls{};
        Result<u32> operator()(Endpoint, Endpoint) { ++calls; check(!table.listen(local, 1) && !table.retire(), "ISS callback cannot reenter mutating owner"); return u32{12}; }
    } source{callback};
    auto in = callback.ingest(packet(local, peer, 1, 0, tcp_syn).view(), 100, source);
    check(bool(in) && in.value().allocated && source.calls == 1, "ISS callback copied result and ownership acquisition");
    check(!callback.tick(99), "table clock spans all endpoint slots");
    check(bool(callback.release(listener, 100)), "callback endpoint cleanup");
}
struct FakeTransport
{
    std::array<Packet, 16> incoming{}, outgoing{}; usize in_count{}, in_head{}, out_count{}, calls{}, reaps{};
    bool reject{}, fail{}, bad_credit{};
    Result<ExecutorTransmitState> reap(usize) { ++reaps; return ExecutorTransmitState{0, bad_credit ? usize{9} : usize{8}}; }
    Result<usize> receive(std::span<std::byte> output)
    {
        if (!in_count) return Status::would_block();
        auto &p = incoming[in_head++]; --in_count;
        if (p.length > output.size()) return Status::overflow();
        for (usize i = 0; i < p.length; ++i) output[i] = p.bytes[i];
        return p.length;
    }
    Status try_transmit(Bytes input)
    {
        ++calls; if (fail) return Status::io_error(); if (reject) return Status::would_block();
        if (out_count == outgoing.size() || input.size() > outgoing[0].bytes.size()) return Status::no_memory();
        auto &p = outgoing[out_count++]; p.length = input.size();
        for (usize i = 0; i < input.size(); ++i) p.bytes[i] = input[i];
        return Status::success();
    }
    void add(Packet p) { check(in_head + in_count < incoming.size(), "fake RX bounded capacity"); incoming[in_head + in_count++] = p; }
};
void executor_controls()
{
    Small table{{13}}; Iss iss; auto h = table.connect(local, peer, 10, 0).value();
    Executor<Small> executor; FakeTransport driver; driver.reject = true;
    check(bool(executor.poll(table, driver, iss, 0)), "executor rejected copied TX is recoverable");
    check(!table.state(h).value().transport.flight && table.state(h).value().transport.send_next == 10 && !table.pools().prepared, "rejected driver submission drains token without SYN commit");
    driver.reject = false; check(bool(executor.poll(table, driver, iss, 1)), "executor actual copied SYN submission");
    auto syn = driver.outgoing[0].parsed(); check(syn.fields.flags == tcp_syn && table.state(h).value().transport.flight, "executor submitted real SYN packet");
    driver.add(packet(peer, local, 20, 11, tcp_syn | tcp_ack));
    check(bool(executor.poll(table, driver, iss, 2)), "executor copied peer handshake ingress");
    check(table.state(h).value().transport.state == State::established && driver.out_count == 2, "executor emits final ACK from real bytes");
    std::array<std::byte, 3> content{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    check(table.write(h, content).value() == 3 && bool(executor.poll(table, driver, iss, 3)), "executor copied stream transmit");
    auto data = driver.outgoing[2].parsed(); check(data.payload.size() == 3 && table.state(h).value().transport.send_buffered == 3, "executor TX remains unacknowledged");
    auto corrupted = packet(peer, local, 21, 14, tcp_ack); corrupted.bytes[36] ^= std::byte{1}; driver.add(corrupted);
    check(bool(executor.poll(table, driver, iss, 4)) && executor.state().dropped == 1 && !executor.state().retired, "malformed network packet drops without retiring healthy link");
    driver.add(packet(peer, local, 21, 14, tcp_ack)); check(bool(executor.poll(table, driver, iss, 5)), "executor validated peer cumulative ACK");
    check(table.state(h).value().transport.send_buffered == 0, "peer ACK releases queued bytes");
    const auto reaps = driver.reaps; check(!executor.poll(table, driver, iss, 4) && driver.reaps == reaps, "backward poll timestamp rejected before driver mutation");
    check(bool(table.write(h, content)), "fatal-TX control data queued"); driver.fail = true;
    check(!executor.poll(table, driver, iss, 6) && executor.state().retired && !table.pools().prepared, "fatal transport drains rejected token then retires endpoint ownership");
    check(table.readiness(h).value().terminal && bool(table.release(h, 6)) && table.pools().connections == 0, "retired canonical owner releases safely");
    Small credits{{14}}; Executor<Small> bad; FakeTransport invalid; invalid.bad_credit = true;
    check(!bad.poll(credits, invalid, iss, 0) && credits.pools().retired && !invalid.calls, "invalid transport credit fails before submission");
}
void ownership_stress()
{
    Small table{{15}}; Iss iss;
    for (u32 iteration = 0; iteration < 80; ++iteration)
    {
        const u64 base = u64{iteration} * 10; auto h = establish(table, iss, 0xfffffff0u + iteration, 500 + iteration, base);
        std::array<std::byte, 25> data{}; for (usize i = 0; i < data.size(); ++i) data[i] = std::byte{static_cast<u8>(i + iteration)};
        check(table.write(h, data).value() == data.size(), "stress copied stream enqueue");
        usize acknowledged{}; u64 transfer_time = base + 2;
        while (acknowledged < data.size())
        {
            auto tx = take(table, transfer_time++); auto f = tx.parsed();
            for (usize i = 0; i < f.payload.size(); ++i) check(byte(f.payload, i) == static_cast<u8>(acknowledged + i + iteration), "independent endpoint stream byte oracle");
            acknowledged += f.payload.size(); ingress(table, packet(peer, local, 501 + iteration,
                f.fields.sequence + static_cast<u32>(f.payload.size()), tcp_ack), transfer_time++, iss);
        }
        check(bool(table.abort(h, base + 6)) && bool(table.release(h, base + 6)) && table.pools().connections == 0, "stress abort/release drains owned incarnation");
        check(!table.state(h), "stress old handle cannot observe reused slot");
    }
}
}
int main()
{
    handle_and_packet_lifecycle(); passive_queues(); listener_close_selected_child(); streams_readiness_and_retirement();
    selected_data_close_and_time_wait(); quotas_aliases_and_callback(); executor_controls(); ownership_stress();
    std::printf("TCP_ENDPOINTS_HOSTED_PASS checks=%zu guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
