#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace ok::net
{

inline constexpr usize max_packet_size = 1500;
inline constexpr usize max_datagrams = 16;
inline constexpr usize max_tcp_listeners = 8;

struct Ipv4Address
{
    std::array<u8, 4> octets{127, 0, 0, 1};

    [[nodiscard]] constexpr bool operator==(const Ipv4Address &other) const
    {
        return octets == other.octets;
    }
};

struct UdpEndpoint
{
    Ipv4Address address{};
    u16 port{0};
};

struct UdpDatagram
{
    UdpEndpoint source{};
    UdpEndpoint destination{};
    std::array<std::byte, max_packet_size> payload{};
    usize payload_size{0};
};

enum class TcpState : u8
{
    closed,
    listen,
    syn_sent,
    established,
};

struct TcpConnection
{
    UdpEndpoint local{};
    UdpEndpoint remote{};
    TcpState state{TcpState::closed};
};

struct NetworkStats
{
    u64 ipv4_tx{0};
    u64 ipv4_rx{0};
    u64 udp_tx{0};
    u64 udp_rx{0};
    u64 tcp_connects{0};
    u64 dropped{0};
};

class NetworkStack final
{
  public:
    Status initialize(Ipv4Address local);
    [[nodiscard]] bool initialized() const
    {
        return initialized_;
    }
    [[nodiscard]] Ipv4Address local_address() const
    {
        return local_;
    }
    [[nodiscard]] NetworkStats stats() const
    {
        return stats_;
    }
    [[nodiscard]] static u16 checksum(std::span<const std::byte> data);
    Status send_udp(UdpEndpoint source, UdpEndpoint destination, std::span<const std::byte> payload);
    Result<UdpDatagram> receive_udp();
    Status listen_tcp(u16 port);
    Result<TcpConnection> connect_tcp(UdpEndpoint remote, u16 local_port);
    [[nodiscard]] usize udp_queued() const
    {
        return udp_rx_.size();
    }
    [[nodiscard]] usize tcp_listener_count() const
    {
        return tcp_listeners_.size();
    }

  private:
    [[nodiscard]] bool is_local(Ipv4Address address) const;
    [[nodiscard]] bool has_tcp_listener(u16 port) const;

    bool initialized_{false};
    Ipv4Address local_{};
    NetworkStats stats_{};
    StaticQueue<UdpDatagram, max_datagrams> udp_rx_{};
    StaticVector<u16, max_tcp_listeners> tcp_listeners_{};
};

[[nodiscard]] std::string_view tcp_state_name(TcpState state);

} // namespace ok::net
