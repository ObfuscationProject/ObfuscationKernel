#include "ok/net/net.hpp"

namespace ok::net
{
namespace
{

void write_be16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>((value >> 8) & 0xffu);
    out[offset + 1] = static_cast<std::byte>(value & 0xffu);
}

void copy_payload(std::span<std::byte> destination, std::span<const std::byte> source)
{
    for (usize i = 0; i < source.size(); ++i)
    {
        destination[i] = source[i];
    }
}

} // namespace

Status NetworkStack::initialize(Ipv4Address local)
{
    local_ = local;
    stats_ = {};
    udp_rx_ = {};
    tcp_listeners_ = {};
    initialized_ = true;
    return Status::success();
}

u16 NetworkStack::checksum(std::span<const std::byte> data)
{
    u32 sum = 0;
    for (usize i = 0; i < data.size(); i += 2)
    {
        u16 word = static_cast<u16>(std::to_integer<u16>(data[i]) << 8);
        if (i + 1 < data.size())
        {
            word = static_cast<u16>(word | std::to_integer<u16>(data[i + 1]));
        }
        sum += word;
        while ((sum >> 16) != 0)
        {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    return static_cast<u16>(~sum & 0xffffu);
}

Status NetworkStack::send_udp(UdpEndpoint source, UdpEndpoint destination, std::span<const std::byte> payload)
{
    if (!initialized_)
    {
        return Status::not_initialized("network stack is not initialized");
    }
    if (payload.size() + 28 > max_packet_size)
    {
        return Status::overflow("UDP payload exceeds MTU");
    }

    std::array<std::byte, max_packet_size> packet{};
    const auto total_length = static_cast<u16>(payload.size() + 28);
    packet[0] = std::byte{0x45};
    packet[8] = std::byte{64};
    packet[9] = std::byte{17};
    write_be16(packet, 2, total_length);
    write_be16(packet, 6, 0x4000);
    for (usize i = 0; i < 4; ++i)
    {
        packet[12 + i] = static_cast<std::byte>(source.address.octets[i]);
        packet[16 + i] = static_cast<std::byte>(destination.address.octets[i]);
    }
    write_be16(packet, 10, checksum(std::span<const std::byte>(packet.data(), 20)));
    write_be16(packet, 20, source.port);
    write_be16(packet, 22, destination.port);
    write_be16(packet, 24, static_cast<u16>(payload.size() + 8));
    copy_payload(std::span<std::byte>(packet.data() + 28, payload.size()), payload);

    ++stats_.ipv4_tx;
    ++stats_.udp_tx;
    if (!is_local(destination.address))
    {
        ++stats_.dropped;
        return Status::unsupported("network stack currently has only loopback routing");
    }

    UdpDatagram datagram{};
    datagram.source = source;
    datagram.destination = destination;
    datagram.payload_size = payload.size();
    copy_payload(std::span<std::byte>(datagram.payload.data(), payload.size()), payload);
    if (auto status = udp_rx_.push(datagram); !status.ok())
    {
        ++stats_.dropped;
        return status;
    }
    ++stats_.ipv4_rx;
    ++stats_.udp_rx;
    return Status::success();
}

Result<UdpDatagram> NetworkStack::receive_udp()
{
    if (!initialized_)
    {
        return Status::not_initialized("network stack is not initialized");
    }
    return udp_rx_.pop();
}

Status NetworkStack::listen_tcp(u16 port)
{
    if (!initialized_)
    {
        return Status::not_initialized("network stack is not initialized");
    }
    if (port == 0)
    {
        return Status::invalid_argument("TCP listener port must be non-zero");
    }
    if (has_tcp_listener(port))
    {
        return Status::already_exists("TCP listener already exists");
    }
    return tcp_listeners_.push_back(port);
}

Result<TcpConnection> NetworkStack::connect_tcp(UdpEndpoint remote, u16 local_port)
{
    if (!initialized_)
    {
        return Status::not_initialized("network stack is not initialized");
    }
    if (!is_local(remote.address))
    {
        ++stats_.dropped;
        return Status::unsupported("TCP routing currently supports loopback only");
    }
    if (!has_tcp_listener(remote.port))
    {
        return Status::not_found("TCP listener not found");
    }
    ++stats_.tcp_connects;
    ++stats_.ipv4_tx;
    ++stats_.ipv4_rx;
    return TcpConnection{
        .local = UdpEndpoint{.address = local_, .port = local_port},
        .remote = remote,
        .state = TcpState::established,
    };
}

bool NetworkStack::is_local(Ipv4Address address) const
{
    return address == local_ || address == Ipv4Address{{127, 0, 0, 1}};
}

bool NetworkStack::has_tcp_listener(u16 port) const
{
    for (const auto listener : tcp_listeners_)
    {
        if (listener == port)
        {
            return true;
        }
    }
    return false;
}

std::string_view tcp_state_name(TcpState state)
{
    switch (state)
    {
    case TcpState::closed:
        return "closed";
    case TcpState::listen:
        return "listen";
    case TcpState::syn_sent:
        return "syn-sent";
    case TcpState::established:
        return "established";
    }
    return "unknown";
}

} // namespace ok::net
