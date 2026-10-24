#include "ok/net/native_udp_executor.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>

namespace
{
using namespace ok;
using namespace ok::net;
usize checks{};
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "NATIVE_UDP_READINESS_SEQUENCE_MODEL_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #expr); \
    std::abort(); } } while (false)

constexpr NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
                             {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee},
                             {192, 0, 2, 2}, {192, 0, 2, 1}};
using Frame = std::vector<std::byte>;
Frame arp_request()
{
    constexpr std::string_view text = "ffffffffffff02aabbccddee0806000108000604000102aabbccddeec0000201"
                                      "000000000000c0000202000000000000000000000000000000000000";
    auto digit = [](char value) -> u8 { return value <= '9' ? value - '0' : value - 'a' + 10; };
    Frame result;
    for (usize at = 0; at < text.size(); at += 2)
        result.push_back(std::byte{static_cast<u8>(digit(text[at]) * 16 + digit(text[at + 1]))});
    return result;
}
struct Transport
{
    std::vector<Frame> incoming;
    usize head{}, reaps{}, receives{}, transmits{};
    NativeUdpTransmitState tx{0, 8};
    Status failure{};
    Result<NativeUdpTransmitState> reap(usize budget)
    {
        CHECK(budget == 8);
        ++reaps;
        return failure.ok() ? Result<NativeUdpTransmitState>{tx} : Result<NativeUdpTransmitState>{failure};
    }
    Result<usize> receive(std::span<std::byte> frame)
    {
        ++receives;
        if (head == incoming.size()) return Status::would_block("ordinary model queue is empty");
        const auto &bytes = incoming[head++];
        CHECK(bytes.size() <= frame.size());
        std::copy(bytes.begin(), bytes.end(), frame.begin());
        return bytes.size();
    }
    Status try_transmit(std::span<const std::byte>)
    {
        ++transmits;
        return Status::success();
    }
};
static_assert(NativeUdpTransport<Transport>);
static_assert(!std::is_copy_constructible_v<NativeUdpService>);
static_assert(!std::is_move_assignable_v<NativeUdpService>);
static_assert(!std::is_copy_assignable_v<NativeUdpReadinessSequence<>>);

void bounded_counter_controls()
{
    NativeUdpReadinessSequence<3> sequence;
    CHECK(sequence.value() == 0 && !sequence.terminal());
    CHECK(sequence.unchanged(0) && !sequence.unchanged(1));
    for (u64 event = 1; event <= 3; ++event)
    {
        const auto before = sequence.value();
        CHECK(sequence.advance());
        CHECK(sequence.value() == event && !sequence.terminal());
        CHECK(!sequence.unchanged(before) && sequence.unchanged(event));
    }
    CHECK(!sequence.advance());
    CHECK(sequence.value() == 3 && sequence.terminal());
    CHECK(!sequence.unchanged(3));
    for (usize attempt = 0; attempt < 64; ++attempt)
    {
        CHECK(!sequence.advance());
        CHECK(sequence.value() == 3 && sequence.terminal());
        CHECK(!sequence.unchanged(3));
    }
    NativeUdpReadinessSequence<1> one;
    CHECK(one.advance() && one.value() == 1 && !one.terminal());
    CHECK(!one.advance() && one.value() == 1 && one.terminal());
}

void explicit_capacity_admission()
{
    NativeUdpReadinessSequence<3> selected{NativeUdpReadinessCapacity{5}};
    CHECK(selected.value() == 0 && !selected.terminal());
    for (u64 event = 1; event <= 5; ++event)
    {
        CHECK(selected.advance());
        CHECK(selected.value() == event && !selected.terminal());
    }
    CHECK(!selected.advance() && selected.value() == 5 && selected.terminal());
    NativeUdpReadinessSequence<> zero{NativeUdpReadinessCapacity{0}};
    CHECK(zero.value() == 0 && zero.terminal() && !zero.unchanged(0));
    CHECK(!zero.advance() && zero.value() == 0);
    NativeUdpService disabled{NativeUdpReadinessCapacity{0}};
    CHECK(disabled.status().code() == StatusCode::io_error);
    CHECK(disabled.configure(link).code() == StatusCode::io_error);
    CHECK(!disabled.configured() && disabled.active_sockets() == 0);
    CHECK(disabled.open().status().code() == StatusCode::io_error);
    NativeUdpService ordinary;
    CHECK(ordinary.configure(link).ok());
    auto socket = ordinary.open();
    CHECK(socket.ok());
    NativeUdpExecutor executor{NativeUdpReadinessCapacity{0}};
    Transport transport;
    CHECK(executor.state().failed && executor.state().readiness_sequence == 0);
    CHECK(executor.status().code() == StatusCode::io_error);
    CHECK(!executor.readiness(ordinary, socket.value()).value().may_wait(0, 0));
    CHECK(executor.poll(ordinary, transport).code() == StatusCode::io_error);
    CHECK(transport.reaps == 0 && transport.receives == 0 && transport.transmits == 0);
    CHECK(ordinary.close(socket.value()).ok());
}

void endpoint_publication()
{
    NativeUdpService engine, peer;
    NativeUdpExecutor executor;
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto first = engine.open(), second = engine.open(), sender = peer.open();
    CHECK(first.ok() && second.ok() && sender.ok());
    CHECK(engine.bind(first.value(), {}, 54321).ok());
    CHECK(engine.bind(second.value(), {}, 54320).ok());
    CHECK(engine.bind(first.value(), {}, 54321).code() == StatusCode::already_exists);
    auto before = executor.readiness(engine, first.value());
    CHECK(before.ok() && before.value().may_wait(0, 0));
    CHECK(before.value().receive.sequence == 0);
    std::array<std::byte, NativeUdpService::frame_capacity> frame{}, reply{};
    CHECK(!engine.build_datagram(first.value(), link.local_ip, 12345, {}, frame));
    CHECK(engine.readiness(first.value()).value().sequence == 0);
    const auto arp = arp_request();
    CHECK(engine.ingest(arp, reply).value().kind == NativeUdpFrameKind::arp_reply);
    CHECK(engine.readiness(first.value()).value().sequence == 0);
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    auto published = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(published.ok() && published.value().queued_handle == first.value());
    auto first_ready = engine.readiness(first.value());
    auto second_ready = engine.readiness(second.value());
    CHECK(first_ready.ok() && first_ready.value().sequence == 1 && first_ready.value().readable());
    CHECK(!first_ready.value().may_wait(1));
    CHECK(second_ready.ok() && second_ready.value().sequence == 1 && !second_ready.value().readable());
    CHECK(!second_ready.value().may_wait(0) && second_ready.value().may_wait(1));
    CHECK(engine.peek(first.value()).value().payload.empty());
    CHECK(engine.consume(first.value()).ok());
    CHECK(engine.readiness(first.value()).value().sequence == 2);
    CHECK(engine.readiness(first.value()).value().may_wait(2));
    CHECK(engine.consume(first.value()).code() == StatusCode::would_block);
    CHECK(engine.readiness(first.value()).value().sequence == 2);
    CHECK(engine.ingest({}, reply).value().kind == NativeUdpFrameKind::dropped);
    CHECK(engine.readiness(first.value()).value().sequence == 2);
    auto published_again = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(published_again.ok() && engine.readiness(first.value()).value().sequence == 3);
    CHECK(engine.ingest({frame.data(), built.value()}, reply).ok());
    CHECK(engine.readiness(first.value()).value().sequence == 4);
    CHECK(engine.ingest({frame.data(), built.value()}, reply).value().kind == NativeUdpFrameKind::dropped);
    CHECK(engine.readiness(first.value()).value().sequence == 4);
    CHECK(engine.consume(first.value()).ok() && engine.consume(first.value()).ok());
    CHECK(engine.readiness(first.value()).value().sequence == 6);
    CHECK(engine.close(first.value()).ok());
    CHECK(!executor.readiness(engine, first.value()));
    auto reopened = engine.open();
    CHECK(reopened.ok() && reopened.value() != first.value());
    CHECK(engine.readiness(reopened.value()).value().sequence == 6);
    CHECK(engine.close(reopened.value()).ok() && engine.close(second.value()).ok());
    CHECK(peer.close(sender.value()).ok());
}

void executor_changes_and_terminal_drain()
{
    NativeUdpService engine, peer;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 0 && transport.reaps == 0);
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto receiver = engine.open(), sender = peer.open();
    CHECK(receiver.ok() && sender.ok());
    CHECK(engine.bind(receiver.value(), {}, 54321).ok());
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 1 && executor.state().transmit_credit == 8);
    auto snapshot = executor.readiness(engine, receiver.value());
    CHECK(snapshot.ok() && snapshot.value().may_wait(0, 1));
    CHECK(!snapshot.value().may_wait(0, 0));
    const auto calls = transport.reaps + transport.receives + transport.transmits;
    for (usize query = 0; query < 64; ++query)
    {
        auto copied = executor.readiness(engine, receiver.value());
        CHECK(copied.ok() && copied.value().may_wait(0, 1));
        CHECK(transport.reaps + transport.receives + transport.transmits == calls);
    }
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 1);
    transport.tx.credit = 0;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 2 && executor.state().transmit_credit == 0);
    CHECK(!executor.readiness(engine, receiver.value()).value().may_wait(0, 1));
    CHECK(executor.readiness(engine, receiver.value()).value().may_wait(0, 2));
    transport.tx.credit = 7;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 3 && executor.state().transmit_credit == 7);
    std::array<std::byte, NativeUdpService::frame_capacity> frame{};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    transport.incoming.emplace_back(frame.begin(), frame.begin() + built.value());
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 3);
    CHECK(!executor.readiness(engine, receiver.value()).value().may_wait(1, 3));
    transport.failure = Status::fault("ordinary transport retirement");
    CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
    CHECK(executor.state().readiness_sequence == 4 && executor.state().failed);
    auto terminal = executor.readiness(engine, receiver.value());
    CHECK(terminal.ok() && terminal.value().receive.readable());
    CHECK(!terminal.value().may_wait(1, 4));
    CHECK(engine.peek(receiver.value()).ok() && engine.consume(receiver.value()).ok());
    terminal = executor.readiness(engine, receiver.value());
    CHECK(terminal.ok() && !terminal.value().receive.readable());
    CHECK(!terminal.value().may_wait(2, 4));
    const auto retired_calls = transport.reaps + transport.receives + transport.transmits;
    for (usize turn = 0; turn < 64; ++turn)
    {
        CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
        CHECK(executor.state().readiness_sequence == 4);
        CHECK(transport.reaps + transport.receives + transport.transmits == retired_calls);
    }
    CHECK(engine.close(receiver.value()).ok() && engine.active_sockets() == 0);
    CHECK(!executor.readiness(engine, receiver.value()));
    CHECK(peer.close(sender.value()).ok());
}

void ordinary_service_exhaustion()
{
    // Fresh ordinary owners select a smaller immutable capacity, always at zero.
    NativeUdpService engine{NativeUdpReadinessCapacity{3}}, peer;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto receiver = engine.open(), other = engine.open(), sender = peer.open();
    CHECK(receiver.ok() && other.ok() && sender.ok());
    CHECK(engine.bind(receiver.value(), {}, 54321).ok());
    std::array<std::byte, NativeUdpService::frame_capacity> frame{}, reply{};
    std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}};
    std::array<std::byte, 2> second{std::byte{4}, std::byte{5}};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, first, frame);
    CHECK(built.ok() && engine.ingest({frame.data(), built.value()}, reply).ok());
    built = peer.build_datagram(sender.value(), link.local_ip, 54321, second, frame);
    CHECK(built.ok() && engine.ingest({frame.data(), built.value()}, reply).ok());
    CHECK(engine.readiness(receiver.value()).value().sequence == 2);
    CHECK(engine.consume(receiver.value()).ok());
    auto before = engine.readiness(receiver.value());
    CHECK(before.ok() && before.value().sequence == 3 && !before.value().terminal);
    CHECK(before.value().queued_datagrams == 1);
    const auto retained = engine.peek(receiver.value());
    CHECK(retained.ok() && retained.value().payload.size() == second.size());
    CHECK(std::equal(second.begin(), second.end(), retained.value().payload.begin()));
    CHECK(engine.ingest({frame.data(), built.value()}, reply).status().code() == StatusCode::io_error);
    auto after = engine.readiness(receiver.value());
    CHECK(after.ok() && after.value().sequence == 3 && after.value().terminal);
    CHECK(after.value().queued_datagrams == 1 && !after.value().may_wait(3));
    CHECK(engine.status().code() == StatusCode::io_error);
    CHECK(engine.active_sockets() == 2 && !engine.open());
    CHECK(engine.bind(other.value(), {}, 54320).code() == StatusCode::io_error);
    CHECK(engine.configure(link).code() == StatusCode::io_error);
    CHECK(engine.build_datagram(receiver.value(), link.peer_ip, 12345, {}, frame).status().code() == StatusCode::io_error);
    CHECK(engine.ingest({}, reply).status().code() == StatusCode::io_error);
    CHECK(engine.readiness(receiver.value()).value().sequence == 3);
    const auto preserved = engine.peek(receiver.value());
    CHECK(preserved.ok() && std::equal(second.begin(), second.end(), preserved.value().payload.begin()));
    CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
    CHECK(transport.reaps == 0 && transport.receives == 0 && transport.transmits == 0);
    CHECK(executor.state().failed && executor.state().transmit_credit == 0);
    CHECK(!executor.readiness(engine, receiver.value()).value().may_wait(3, 1));
    CHECK(engine.consume(receiver.value()).ok());
    after = engine.readiness(receiver.value());
    CHECK(after.ok() && !after.value().readable() && after.value().terminal && after.value().sequence == 3);
    CHECK(!after.value().may_wait(3));
    CHECK(engine.peek(receiver.value()).status().code() == StatusCode::io_error);
    CHECK(engine.consume(receiver.value()).code() == StatusCode::io_error);
    CHECK(engine.close(receiver.value()).ok() && engine.close(other.value()).ok());
    CHECK(engine.active_sockets() == 0 && !engine.open());
    CHECK(!executor.readiness(engine, receiver.value()));
    CHECK(peer.close(sender.value()).ok());

    NativeUdpService consumed{NativeUdpReadinessCapacity{1}}, another_peer;
    CHECK(consumed.configure(link).ok());
    CHECK(another_peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto handle = consumed.open(), peer_handle = another_peer.open();
    CHECK(handle.ok() && peer_handle.ok() && consumed.bind(handle.value(), {}, 54321).ok());
    built = another_peer.build_datagram(peer_handle.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok() && consumed.ingest({frame.data(), built.value()}, reply).ok());
    CHECK(consumed.readiness(handle.value()).value().sequence == 1 && consumed.status().ok());
    CHECK(consumed.consume(handle.value()).ok());
    auto empty = consumed.readiness(handle.value());
    CHECK(empty.ok() && empty.value().sequence == 1 && empty.value().terminal && !empty.value().readable());
    CHECK(!empty.value().may_wait(1));
    CHECK(consumed.close(handle.value()).ok() && another_peer.close(peer_handle.value()).ok());
}

void ordinary_executor_exhaustion()
{
    NativeUdpService engine, peer;
    NativeUdpExecutor executor{NativeUdpReadinessCapacity{1}};
    Transport transport;
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto receiver = engine.open(), sender = peer.open();
    CHECK(receiver.ok() && sender.ok() && engine.bind(receiver.value(), {}, 54321).ok());
    CHECK(executor.poll(engine, transport).ok() && executor.state().readiness_sequence == 1);
    std::array<std::byte, NativeUdpService::frame_capacity> frame{};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    transport.incoming.emplace_back(frame.begin(), frame.begin() + built.value());
    transport.tx.credit = 7;
    CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
    CHECK(executor.state().failed && !executor.state().active && executor.state().transmit_credit == 0);
    CHECK(executor.state().failure_cause == StatusCode::overflow && executor.state().readiness_sequence == 1);
    CHECK(engine.peek(receiver.value()).ok());
    CHECK(!executor.readiness(engine, receiver.value()).value().may_wait(1, 1));
    CHECK(engine.consume(receiver.value()).ok());
    CHECK(!executor.readiness(engine, receiver.value()).value().may_wait(2, 1));
    const auto calls = transport.reaps + transport.receives + transport.transmits;
    for (usize attempt = 0; attempt < 64; ++attempt)
    {
        CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
        CHECK(executor.state().readiness_sequence == 1 && executor.state().failed);
        CHECK(transport.reaps + transport.receives + transport.transmits == calls);
    }
    CHECK(engine.close(receiver.value()).ok() && peer.close(sender.value()).ok());

    NativeUdpService isolated;
    NativeUdpExecutor coincident{NativeUdpReadinessCapacity{1}};
    Transport failed;
    CHECK(isolated.configure(link).ok());
    CHECK(coincident.poll(isolated, failed).ok());
    failed.failure = Status::fault("ordinary first device cause survives saturation");
    CHECK(coincident.poll(isolated, failed).code() == StatusCode::io_error);
    CHECK(coincident.state().readiness_sequence == 1 && coincident.state().failed);
    CHECK(coincident.state().failure_cause == StatusCode::fault);
}

void control_diagnostics_and_cached_credit()
{
    NativeUdpService engine;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(engine.configure(link).ok());
    auto receiver = engine.open();
    CHECK(receiver.ok());
    transport.tx.credit = 0;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 1);
    const auto arp = arp_request();
    transport.incoming = {arp, arp, arp};
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 1 && executor.state().pending_control == 2);
    CHECK(executor.state().dropped_control == 1 && engine.readiness(receiver.value()).value().sequence == 0);
    transport.tx.credit = 8;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 2 && executor.state().transmit_credit == 7);
    CHECK(executor.state().pending_control == 1);
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 2 && executor.state().transmit_credit == 7);
    CHECK(executor.state().pending_control == 0);
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().readiness_sequence == 3 && executor.state().transmit_credit == 8);
    CHECK(engine.close(receiver.value()).ok());
}
}

int main()
{
    bounded_counter_controls();
    explicit_capacity_admission();
    endpoint_publication();
    executor_changes_and_terminal_drain();
    ordinary_service_exhaustion();
    ordinary_executor_exhaustion();
    control_diagnostics_and_cached_credit();
    std::printf("NATIVE_UDP_READINESS_SEQUENCE_MODEL_PASS checks=%zu\n", checks);
}
