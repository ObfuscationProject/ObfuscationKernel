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
    std::fprintf(stderr, "NATIVE_UDP_EXECUTOR_MODEL_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #expr); \
    std::abort(); } } while (false)

constexpr NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
                             {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee},
                             {192, 0, 2, 2}, {192, 0, 2, 1}};
using Frame = std::vector<std::byte>;
Frame hex(const char *text)
{
    auto value = [](char digit) -> u8 {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        std::abort();
    };
    Frame bytes;
    for (usize at = 0; text[at]; at += 2)
    {
        CHECK(text[at + 1]);
        bytes.push_back(std::byte{static_cast<u8>(value(text[at]) * 16 + value(text[at + 1]))});
    }
    return bytes;
}
Frame arp(u8 suffix = 0xee)
{
    auto result = hex("ffffffffffff02aabbccddee0806000108000604000102aabbccddeec0000201"
                      "000000000000c0000202000000000000000000000000000000000000");
    // Keep the exact admitted peer identity; distinguish controls by padding.
    result.back() = std::byte{suffix};
    return result;
}

struct Transport
{
    std::vector<Frame> incoming, submitted;
    usize head{}, reap_calls{}, receive_calls{}, submit_calls{}, last_budget{};
    NativeUdpTransmitState tx{0, 8};
    Status reap_status{}, receive_status{}, submit_status{};
    usize forced_length{};

    Result<NativeUdpTransmitState> reap(usize budget)
    {
        ++reap_calls;
        last_budget = budget;
        return reap_status.ok() ? Result<NativeUdpTransmitState>{tx} : Result<NativeUdpTransmitState>{reap_status};
    }
    Result<usize> receive(std::span<std::byte> frame)
    {
        ++receive_calls;
        if (!receive_status) return receive_status;
        if (forced_length) return forced_length;
        if (head == incoming.size()) return Status::not_found("model receive queue is empty");
        const auto &bytes = incoming[head++];
        CHECK(bytes.size() <= frame.size());
        std::copy(bytes.begin(), bytes.end(), frame.begin());
        return bytes.size();
    }
    Status try_transmit(std::span<const std::byte> frame)
    {
        ++submit_calls;
        if (!submit_status) return submit_status;
        submitted.emplace_back(frame.begin(), frame.end());
        return Status::success();
    }
};
static_assert(NativeUdpTransport<Transport>);
static_assert(!std::is_copy_constructible_v<NativeUdpExecutor>);
static_assert(!std::is_move_constructible_v<NativeUdpExecutor>);
static_assert(!std::is_copy_assignable_v<NativeUdpExecutor>);
static_assert(!std::is_move_assignable_v<NativeUdpExecutor>);

void bounded_ingress()
{
    NativeUdpService engine, peer;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(executor.poll(engine, transport).ok());
    CHECK(!executor.activated() && transport.reap_calls == 0 && transport.receive_calls == 0);
    CHECK(engine.configure(link).ok());
    CHECK(peer.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}).ok());
    auto first = engine.open(), second = engine.open(), sender = peer.open();
    CHECK(first.ok() && second.ok() && sender.ok());
    CHECK(engine.bind(first.value(), {}, 54321).ok());
    CHECK(engine.bind(second.value(), {}, 54320).ok());
    CHECK(peer.bind(sender.value(), {}, 12345).ok());
    std::array<std::byte, NativeUdpService::frame_capacity> frame{};
    auto built = peer.build_datagram(sender.value(), link.local_ip, 54321, {}, frame);
    CHECK(built.ok());
    transport.incoming.emplace_back(frame.begin(), frame.begin() + built.value());
    std::array<std::byte, 3> payload{std::byte{0}, std::byte{1}, std::byte{0x11}};
    built = peer.build_datagram(sender.value(), link.local_ip, 54320, payload, frame);
    CHECK(built.ok());
    transport.incoming.emplace_back(frame.begin(), frame.begin() + built.value());
    for (usize index = 0; index < 7; ++index) transport.incoming.push_back(arp());
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.activated() && transport.last_budget == 8 && transport.reap_calls == 1);
    CHECK(transport.receive_calls == 8 && transport.head == 8 && transport.submit_calls == 1);
    CHECK(executor.state().pending_control == 1 && executor.state().dropped_control == 4);
    CHECK(executor.state().transmit_credit == 7);
    CHECK(engine.readiness(first.value()).value().readable());
    CHECK(engine.peek(first.value()).value().payload.empty());
    CHECK(engine.readiness(second.value()).value().readable());
    auto received = engine.peek(second.value());
    CHECK(received.ok() && received.value().payload.size() == payload.size());
    CHECK(std::equal(payload.begin(), payload.end(), received.value().payload.begin()));
    const auto expected = hex("02aabbccddee02112233445508060001080006040002021122334455c0000202"
                              "02aabbccddeec0000201000000000000000000000000000000000000");
    CHECK(transport.submitted.front() == expected);
    const auto calls = transport.receive_calls + transport.reap_calls + transport.submit_calls;
    for (usize query = 0; query < 64; ++query)
    {
        CHECK(executor.state().active && executor.status().ok());
        CHECK(executor.state().pending_control == 1 && executor.state().transmit_credit == 7);
        CHECK(transport.receive_calls + transport.reap_calls + transport.submit_calls == calls);
        CHECK(engine.readiness(first.value()).value().queued_datagrams == 1);
    }
    CHECK(executor.poll(engine, transport).ok());
    CHECK(transport.head == 9 && transport.receive_calls == 10 && transport.submit_calls == 2);
    CHECK(executor.state().pending_control == 1 && executor.state().dropped_control == 4);
    CHECK(transport.submitted[0] == transport.submitted[1]);
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().pending_control == 0 && transport.submit_calls == 3);
    CHECK(engine.consume(first.value()).ok() && engine.consume(second.value()).ok());
    CHECK(engine.close(first.value()).ok() && engine.close(second.value()).ok());
    CHECK(peer.close(sender.value()).ok());
}

void pressure_and_faults()
{
    NativeUdpService engine;
    NativeUdpExecutor executor;
    Transport transport;
    CHECK(engine.configure(link).ok());
    transport.tx.credit = 0;
    transport.incoming = {arp(), arp(), arp()};
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().pending_control == 2 && executor.state().dropped_control == 1);
    CHECK(transport.submit_calls == 0);
    transport.tx.credit = 8;
    transport.submit_status = Status::would_block("model TX is full");
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().transmit_credit == 0 && executor.state().pending_control == 2);
    CHECK(transport.submit_calls == 1 && transport.submitted.empty());
    transport.submit_status = Status::success();
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().pending_control == 1 && transport.submitted.size() == 1);
    transport.receive_status = Status::would_block("model RX has no frame");
    CHECK(executor.poll(engine, transport).ok());
    CHECK(executor.state().pending_control == 0 && transport.submitted.size() == 2);
    transport.receive_status = Status::fault("model device retired");
    CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
    CHECK(executor.state().failed && !executor.state().active && executor.state().transmit_credit == 0);
    CHECK(executor.state().failure_cause == StatusCode::fault);
    const auto calls = transport.receive_calls + transport.reap_calls + transport.submit_calls;
    for (usize turn = 0; turn < 64; ++turn)
    {
        CHECK(executor.poll(engine, transport).code() == StatusCode::io_error);
        CHECK(transport.receive_calls + transport.reap_calls + transport.submit_calls == calls);
    }
    for (usize case_id = 0; case_id < 6; ++case_id)
    {
        NativeUdpService isolated;
        NativeUdpExecutor isolated_executor;
        Transport isolated_transport;
        CHECK(isolated.configure(link).ok());
        if (case_id == 0) isolated_transport.tx.completed = 9;
        if (case_id == 1) isolated_transport.tx.credit = 9;
        if (case_id == 2) isolated_transport.forced_length = NativeUdpService::frame_capacity + 1;
        if (case_id == 3) isolated_transport.incoming.push_back({});
        if (case_id == 4) isolated_transport.reap_status = Status::not_initialized("model link is absent");
        if (case_id == 5)
        {
            isolated_transport.incoming.push_back(arp());
            isolated_transport.submit_status = Status::io_error("model submit retired");
        }
        CHECK(isolated_executor.poll(isolated, isolated_transport).code() == StatusCode::io_error);
        CHECK(isolated_executor.activated() && isolated_executor.state().failed);
        CHECK(isolated_executor.state().transmit_credit == 0);
        const auto before = isolated_transport.receive_calls + isolated_transport.reap_calls + isolated_transport.submit_calls;
        CHECK(isolated_executor.poll(isolated, isolated_transport).code() == StatusCode::io_error);
        CHECK(before == isolated_transport.receive_calls + isolated_transport.reap_calls + isolated_transport.submit_calls);
        if (case_id == 5) CHECK(isolated_executor.state().pending_control == 1);
    }
}
}

int main()
{
    bounded_ingress();
    pressure_and_faults();
    std::printf("NATIVE_UDP_EXECUTOR_MODEL_PASS checks=%zu\n", checks);
}
