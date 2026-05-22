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
inline constexpr usize max_arp_entries = 8;
inline constexpr usize max_sockets = 16;

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

struct IcmpEchoReply
{
    Ipv4Address source{};
    Ipv4Address destination{};
    u16 identifier{0};
    u16 sequence{0};
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

struct EthernetAddress
{
    std::array<u8, 6> octets{0x02, 0, 0, 0, 0, 1};
};

class VirtioNetDevice final
{
  public:
    Status probe();
    Status start(EthernetAddress mac);
    Status stop();
    [[nodiscard]] bool started() const
    {
        return started_;
    }
    [[nodiscard]] EthernetAddress mac() const
    {
        return mac_;
    }
    Status transmit(std::span<const std::byte> frame);
    Status receive(std::span<const std::byte> frame);
    [[nodiscard]] u64 tx_count() const
    {
        return tx_count_;
    }
    [[nodiscard]] u64 rx_count() const
    {
        return rx_count_;
    }

  private:
    bool probed_{false};
    bool started_{false};
    EthernetAddress mac_{};
    u64 tx_count_{0};
    u64 rx_count_{0};
};

class ArpCache final
{
  public:
    Status learn(Ipv4Address address, EthernetAddress mac);
    Result<EthernetAddress> lookup(Ipv4Address address) const;
    [[nodiscard]] usize size() const
    {
        return entries_.size();
    }

  private:
    struct Entry
    {
        Ipv4Address address{};
        EthernetAddress mac{};
    };

    StaticVector<Entry, max_arp_entries> entries_;
};

enum class SocketType : u8
{
    udp,
    tcp,
};

enum class SocketState : u8
{
    closed,
    created,
    bound,
    listening,
    connected,
};

struct Socket
{
    i32 fd{-1};
    SocketType type{SocketType::udp};
    SocketState state{SocketState::closed};
    UdpEndpoint local{};
    UdpEndpoint remote{};
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
    Result<IcmpEchoReply> send_icmp_echo(Ipv4Address destination, u16 identifier, u16 sequence,
                                         std::span<const std::byte> payload);
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

class SocketTable final
{
  public:
    Status initialize(NetworkStack &stack);
    Result<i32> socket(SocketType type);
    Status bind(i32 fd, UdpEndpoint local);
    Status listen(i32 fd);
    Result<i32> accept(i32 fd);
    Status connect(i32 fd, UdpEndpoint remote);
    Result<usize> sendto(i32 fd, std::span<const std::byte> payload, UdpEndpoint remote);
    Result<UdpDatagram> recvfrom(i32 fd);
    Result<usize> write(i32 fd, std::span<const std::byte> payload);
    Result<usize> read(i32 fd, std::span<std::byte> out);
    Result<u32> poll(i32 fd) const;
    Status shutdown(i32 fd);
    [[nodiscard]] Socket *find(i32 fd);
    [[nodiscard]] const Socket *find(i32 fd) const;

  private:
    NetworkStack *stack_{nullptr};
    StaticVector<Socket, max_sockets> sockets_;
    i32 next_fd_{3};
};

[[nodiscard]] std::string_view tcp_state_name(TcpState state);

} // namespace ok::net
