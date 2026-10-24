#include "ok/net/native_udp.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
using namespace ok;
using namespace ok::net;
usize checks{};
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "NATIVE_UDP_MODEL_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #expr); \
    std::abort(); } } while (false)

std::vector<std::byte> hex(const char *text)
{
    auto value = [](char digit) -> u8 {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        std::abort();
    };
    std::vector<std::byte> bytes;
    for (usize at = 0; text[at]; at += 2)
    {
        CHECK(text[at + 1]);
        bytes.push_back(std::byte{static_cast<u8>(value(text[at]) * 16 + value(text[at + 1]))});
    }
    return bytes;
}
wire::Bytes view(const std::vector<std::byte> &bytes) { return {bytes.data(), bytes.size()}; }

void run()
{
    NativeUdpService engine;
    constexpr NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
                                 {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee},
                                 {192, 0, 2, 2}, {192, 0, 2, 1}};
    CHECK(engine.configure(link).ok());
    CHECK(!engine.configure(link).ok());
    auto first = engine.open(); auto second = engine.open();
    CHECK(first.ok() && second.ok() && first.value() != second.value());
    CHECK(engine.bind(first.value(), {}, 54321).ok());
    CHECK(engine.bind(second.value(), link.local_ip, 1023).code() == StatusCode::invalid_argument);
    CHECK(engine.bind(second.value(), link.local_ip, 54321).code() == StatusCode::already_exists);
    CHECK(engine.bind(second.value(), link.local_ip, 54320).ok());

    const auto arp = hex("ffffffffffff02aabbccddee0806000108000604000102aabbccddeec0000201"
                         "000000000000c0000202000000000000000000000000000000000000");
    const auto arp_expected = hex("02aabbccddee02112233445508060001080006040002021122334455c0000202"
                                  "02aabbccddeec0000201000000000000000000000000000000000000");
    std::array<std::byte, NativeUdpService::frame_capacity> reply{};
    auto handled = engine.ingest(view(arp), reply);
    CHECK(handled.ok() && handled.value().kind == NativeUdpFrameKind::arp_reply);
    CHECK(handled.value().reply_length == arp_expected.size());
    CHECK(std::equal(arp_expected.begin(), arp_expected.end(), reply.begin()));

    const auto request = hex("02112233445502aabbccddee08004500001f123540004011a495c0000201c0000202"
                             "3039d431000b6668000111000000000000000000000000000000");
    handled = engine.ingest(view(request), reply);
    CHECK(handled.ok() && handled.value().kind == NativeUdpFrameKind::udp_queued);
    CHECK(engine.peek(second.value()).status().code() == StatusCode::would_block);
    auto received = engine.peek(first.value());
    CHECK(received.ok() && received.value().source == link.peer_ip && received.value().source_port == 12345);
    CHECK(received.value().payload.size() == 3 && received.value().payload[0] == std::byte{0} &&
          received.value().payload[1] == std::byte{1} && received.value().payload[2] == std::byte{0x11});
    CHECK(engine.ingest(view(request), reply).ok());
    CHECK(engine.ingest(view(request), reply).value().kind == NativeUdpFrameKind::dropped);
    CHECK(engine.dropped_frames() == 1);
    CHECK(engine.consume(first.value()).ok());
    CHECK(engine.consume(first.value()).ok());
    CHECK(engine.consume(first.value()).code() == StatusCode::would_block);

    auto invalid = request; invalid[43] ^= std::byte{1};
    CHECK(engine.ingest(view(invalid), reply).value().kind == NativeUdpFrameKind::dropped);
    CHECK(engine.peek(first.value()).status().code() == StatusCode::would_block);
    CHECK(engine.dropped_frames() == 2);

    const auto payload = hex("616263");
    const auto expected_tx = hex("02aabbccddee02112233445508004500001f90004000401126cac0000202c0000201"
                                 "d4313039000bb306616263000000000000000000000000000000");
    auto built = engine.build_datagram(first.value(), link.peer_ip, 12345, view(payload), reply);
    CHECK(built.ok() && built.value() == expected_tx.size());
    CHECK(std::equal(expected_tx.begin(), expected_tx.end(), reply.begin()));
    const auto old = reply;
    CHECK(engine.build_datagram(first.value(), link.peer_ip, 12345,
        {reply.data(), 3}, reply).status().code() == StatusCode::invalid_argument);
    CHECK(reply == old);
    std::array<std::byte, NativeUdpService::datagram_capacity + 1> oversized{};
    CHECK(engine.build_datagram(first.value(), link.peer_ip, 12345, oversized, reply)
              .status().code() == StatusCode::message_size);
    std::array<std::byte, NativeUdpService::datagram_capacity> maximum{};
    auto maximum_frame = engine.build_datagram(first.value(), link.peer_ip, 12345, maximum, reply);
    CHECK(maximum_frame.ok() && maximum_frame.value() == NativeUdpService::frame_capacity);
    auto ethernet = wire::ethernet({reply.data(), maximum_frame.value()});
    CHECK(ethernet.ok());
    auto ip = wire::ipv4(ethernet.value().payload);
    CHECK(ip.ok());
    auto udp = wire::udp(ip.value());
    CHECK(udp.ok() && udp.value().payload.size() == maximum.size());
    CHECK(engine.close(first.value()).ok());
    CHECK(engine.peek(first.value()).status().code() == StatusCode::bad_descriptor);
    auto reused = engine.open();
    CHECK(reused.ok() && reused.value() != first.value());
    CHECK(engine.bind(reused.value(), link.local_ip, 54321).ok());
    CHECK(engine.close(second.value()).ok());
    CHECK(engine.close(reused.value()).ok());
    CHECK(engine.active_sockets() == 0);
}

void run_readiness()
{
    NativeUdpService engine, peer;
    auto first = engine.open();
    auto second = engine.open();
    CHECK(first.ok() && second.ok());
    CHECK(engine.readiness(first.value()).ok());
    CHECK(!engine.readiness(first.value()).value().readable());
    CHECK(engine.readiness(0).status().code() == StatusCode::bad_descriptor);
    constexpr NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
                                 {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee},
                                 {192, 0, 2, 2}, {192, 0, 2, 1}};
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    CHECK(engine.bind(first.value(), {}, 54321).ok());
    CHECK(engine.bind(second.value(), link.local_ip, 54320).ok());
    auto sender = peer.open();
    CHECK(sender.ok() && peer.bind(sender.value(), {}, 12345).ok());
    std::array<std::byte, NativeUdpService::frame_capacity> frame{}, reply{};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    auto ingress = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().kind == NativeUdpFrameKind::udp_queued);
    CHECK(ingress.value().queued_handle == first.value() && ingress.value().reply_length == 0);
    const auto original_frame = frame;
    const auto original_reply = reply;
    for (usize turn = 0; turn < 64; ++turn)
    {
        const auto state = engine.readiness(first.value());
        CHECK(state.ok() && state.value().readable() && state.value().queued_datagrams == 1);
        CHECK(!engine.readiness(second.value()).value().readable());
        CHECK(engine.peek(first.value()).ok() && engine.peek(first.value()).value().payload.empty());
        CHECK(engine.active_sockets() == 2 && engine.dropped_frames() == 0);
        CHECK(frame == original_frame && reply == original_reply);
    }
    std::array<std::byte, NativeUdpService::datagram_capacity> maximum{};
    for (usize index = 0; index < maximum.size(); ++index)
        maximum[index] = std::byte{static_cast<u8>(index)};
    built = peer.build_datagram(sender.value(), link.local_ip, 54321, maximum, frame);
    CHECK(built.ok());
    ingress = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().queued_handle == first.value());
    CHECK(engine.readiness(first.value()).value().queued_datagrams == 2);
    ingress = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().kind == NativeUdpFrameKind::dropped);
    CHECK(ingress.value().queued_handle == 0);
    CHECK(engine.readiness(first.value()).value().queued_datagrams == 2);
    CHECK(engine.consume(first.value()).ok());
    CHECK(engine.readiness(first.value()).value().queued_datagrams == 1);
    auto data = engine.peek(first.value());
    CHECK(data.ok() && data.value().payload.size() == maximum.size());
    CHECK(std::equal(maximum.begin(), maximum.end(), data.value().payload.begin()));
    CHECK(engine.consume(first.value()).ok());
    CHECK(!engine.readiness(first.value()).value().readable());
    built = peer.build_datagram(sender.value(), link.local_ip, 54320, {}, frame);
    CHECK(built.ok());
    ingress = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().queued_handle == second.value());
    CHECK(engine.readiness(second.value()).value().readable());
    CHECK(!engine.readiness(first.value()).value().readable());
    CHECK(engine.close(first.value()).ok());
    CHECK(engine.readiness(first.value()).status().code() == StatusCode::bad_descriptor);
    auto replacement = engine.open();
    CHECK(replacement.ok() && replacement.value() != first.value());
    CHECK(!engine.readiness(replacement.value()).value().readable());
    CHECK(engine.bind(replacement.value(), {}, 54321).ok());
    built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    ingress = engine.ingest({frame.data(), built.value()}, reply);
    CHECK(ingress.ok() && ingress.value().queued_handle == replacement.value());
    CHECK(engine.readiness(first.value()).status().code() == StatusCode::bad_descriptor);
    CHECK(engine.consume(replacement.value()).ok());
    CHECK(engine.close(replacement.value()).ok());
    CHECK(engine.close(second.value()).ok());
    CHECK(peer.close(sender.value()).ok());
    CHECK(engine.active_sockets() == 0 && peer.active_sockets() == 0);
}
}

int main()
{
    run();
    run_readiness();
    std::printf("NATIVE_UDP_MODEL_PASS checks=%zu\n", checks);
}
