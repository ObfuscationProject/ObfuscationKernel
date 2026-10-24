#include "ok/net/native_udp_poll.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
using namespace ok;
using namespace ok::net;
using Events = posix::ReadyEvents;
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "NATIVE_UDP_POLL_MODEL_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #expression); \
    std::abort(); } } while (false)

struct Vector
{
    usize queued{}, credit{};
    bool terminal{}, active{}, failed{}, readable{true}, writable{true};
    u16 requested{}, expected{};
};

void event_vectors()
{
    // Literal canonical masks: IN=1, OUT=4, ERR=8, NVAL=32.
    constexpr Vector vectors[]{
        {0, 8, false, false, false, true, true, 5, 0},
        {0, 8, false, true, false, true, true, 5, 4},
        {0, 0, false, true, false, true, true, 5, 0},
        {1, 0, false, false, false, true, true, 1, 1},
        {2, 8, false, true, false, true, true, 5, 5},
        {1, 8, true, true, false, true, true, 5, 9},
        {1, 8, false, true, true, true, true, 5, 9},
        {0, 8, true, true, false, true, true, 0, 8},
        {0, 8, false, true, true, true, true, 0, 8},
        {1, 8, false, true, false, false, true, 5, 4},
        {1, 8, false, true, false, true, false, 5, 1},
        {1, 8, false, true, false, true, true, 0, 0},
        {1, 8, false, true, false, true, true, 4, 4},
        {1, 8, false, true, false, true, true, 1, 1},
        {1, 8, true, true, false, false, false, 0, 8},
        {1, 8, false, true, false, true, true, 2, 0},
        {1, 8, true, true, false, true, true, 2, 8},
    };
    for (const auto &vector : vectors)
    {
        NativeUdpReceiveReadiness snapshot{};
        snapshot.receive = {vector.queued, 123, vector.terminal};
        snapshot.executor.active = vector.active;
        snapshot.executor.failed = vector.failed;
        snapshot.executor.transmit_credit = vector.credit;
        snapshot.executor.readiness_sequence = 456;
        auto result = native_udp_descriptor_readiness(snapshot, vector.readable, vector.writable,
            static_cast<Events>(vector.requested));
        CHECK(result.ok());
        CHECK(static_cast<u16>(result.value().events) == vector.expected);
        CHECK(!result.value().console_input);
        CHECK(snapshot.receive.sequence == 123 && snapshot.executor.readiness_sequence == 456);
    }
    const Result<NativeUdpReceiveReadiness> stale = Status::bad_descriptor("ordinary stale endpoint");
    const auto invalid = native_udp_descriptor_readiness(stale, true, true, Events::none);
    CHECK(invalid.ok() && static_cast<u16>(invalid.value().events) == 32);
    const Result<NativeUdpReceiveReadiness> fault = Status::fault("ordinary snapshot failure");
    CHECK(native_udp_descriptor_readiness(fault, true, true, Events::none).status().code() == StatusCode::fault);
}

struct Transport
{
    usize reaps{}, receives{}, transmits{};
    Status failure{};
    Result<NativeUdpTransmitState> reap(usize budget)
    {
        CHECK(budget == 8);
        ++reaps;
        if (!failure) return failure;
        return NativeUdpTransmitState{0, 8};
    }
    Result<usize> receive(std::span<std::byte>)
    {
        ++receives;
        return Status::would_block("ordinary model receive queue empty");
    }
    Status try_transmit(std::span<const std::byte>)
    {
        ++transmits;
        return Status::success();
    }
};
static_assert(NativeUdpTransport<Transport>);

void ordinary_query_purity_and_drain()
{
    constexpr NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
        {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, {192, 0, 2, 2}, {192, 0, 2, 1}};
    NativeUdpService service, peer;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(service.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto receiver = service.open(), sender = peer.open();
    CHECK(receiver.ok() && sender.ok());
    CHECK(service.bind(receiver.value(), {}, 54321).ok());
    CHECK(executor.poll(service, transport).ok());
    std::array<std::byte, NativeUdpService::frame_capacity> frame{}, reply{};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    auto ingress = service.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().queued_handle == receiver.value());
    CHECK(service.peek(receiver.value()).ok() && service.peek(receiver.value()).value().payload.empty());
    const auto before = executor.readiness(service, receiver.value()).value();
    const auto calls = transport.reaps + transport.receives + transport.transmits;
    for (usize query = 0; query < 64; ++query)
    {
        auto snapshot = executor.readiness(service, receiver.value());
        const auto ready = native_udp_descriptor_readiness(snapshot, true, true, Events::readable | Events::writable);
        CHECK(ready.ok() && static_cast<u16>(ready.value().events) == 5);
        CHECK(snapshot.value().receive.sequence == before.receive.sequence);
        CHECK(snapshot.value().executor.readiness_sequence == before.executor.readiness_sequence);
        CHECK(snapshot.value().receive.queued_datagrams == 1);
        CHECK(transport.reaps + transport.receives + transport.transmits == calls);
    }
    transport.failure = Status::fault("ordinary device retirement");
    CHECK(executor.poll(service, transport).code() == StatusCode::io_error);
    auto terminal = native_udp_descriptor_readiness(executor.readiness(service, receiver.value()),
        true, true, Events::readable | Events::writable);
    CHECK(terminal.ok() && static_cast<u16>(terminal.value().events) == 9);
    CHECK(service.consume(receiver.value()).ok());
    terminal = native_udp_descriptor_readiness(executor.readiness(service, receiver.value()),
        true, true, Events::none);
    CHECK(terminal.ok() && static_cast<u16>(terminal.value().events) == 8);
    CHECK(service.close(receiver.value()).ok() && service.active_sockets() == 0);
    auto invalid = native_udp_descriptor_readiness(executor.readiness(service, receiver.value()),
        true, true, Events::readable | Events::writable);
    CHECK(invalid.ok() && static_cast<u16>(invalid.value().events) == 32);
    CHECK(peer.close(sender.value()).ok());
}
}

int main()
{
    event_vectors();
    ordinary_query_purity_and_drain();
    std::printf("NATIVE_UDP_POLL_MODEL_PASS checks=%zu\n", checks);
}
