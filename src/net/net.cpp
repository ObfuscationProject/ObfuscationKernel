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

Status VirtioNetDevice::probe()
{
    probed_ = true;
    return Status::success();
}

Status VirtioNetDevice::start(EthernetAddress mac)
{
    if (!probed_)
    {
        return Status::not_initialized("virtio-net device has not been probed");
    }
    mac_ = mac;
    started_ = true;
    return Status::success();
}

Status VirtioNetDevice::stop()
{
    started_ = false;
    return Status::success();
}

Status VirtioNetDevice::transmit(std::span<const std::byte> frame)
{
    if (!started_)
    {
        return Status::not_initialized("virtio-net device is not started");
    }
    if (frame.empty() || frame.size() > max_packet_size)
    {
        return Status::invalid_argument("invalid Ethernet frame");
    }
    ++tx_count_;
    return Status::success();
}

Status VirtioNetDevice::receive(std::span<const std::byte> frame)
{
    if (!started_)
    {
        return Status::not_initialized("virtio-net device is not started");
    }
    if (frame.empty() || frame.size() > max_packet_size)
    {
        return Status::invalid_argument("invalid Ethernet frame");
    }
    ++rx_count_;
    return Status::success();
}

Status ArpCache::learn(Ipv4Address address, EthernetAddress mac)
{
    for (auto &entry : entries_)
    {
        if (entry.address == address)
        {
            entry.mac = mac;
            return Status::success();
        }
    }
    return entries_.push_back(Entry{.address = address, .mac = mac});
}

Result<EthernetAddress> ArpCache::lookup(Ipv4Address address) const
{
    for (const auto &entry : entries_)
    {
        if (entry.address == address)
        {
            return entry.mac;
        }
    }
    return Status::not_found("ARP entry not found");
}

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

Result<IcmpEchoReply> NetworkStack::send_icmp_echo(Ipv4Address destination, u16 identifier, u16 sequence,
                                                   std::span<const std::byte> payload)
{
    if (!initialized_)
    {
        return Status::not_initialized("network stack is not initialized");
    }
    if (payload.size() + 28 > max_packet_size)
    {
        return Status::overflow("ICMP echo payload exceeds MTU");
    }

    ++stats_.ipv4_tx;
    if (!is_local(destination))
    {
        ++stats_.dropped;
        return Status::unsupported("ICMP echo currently supports loopback only");
    }

    ++stats_.ipv4_rx;
    return IcmpEchoReply{
        .source = destination,
        .destination = local_,
        .identifier = identifier,
        .sequence = sequence,
        .payload_size = payload.size(),
    };
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

Status SocketTable::initialize(NetworkStack &stack)
{
    stack_ = &stack;
    sockets_ = {};
    next_fd_ = 3;
    return Status::success();
}

Result<i32> SocketTable::socket(SocketType type)
{
    if (stack_ == nullptr)
    {
        return Status::not_initialized("socket table is not initialized");
    }
    if (sockets_.full())
    {
        return Status::overflow("socket table is full");
    }
    const auto fd = next_fd_++;
    if (auto status = sockets_.push_back(Socket{.fd = fd, .type = type, .state = SocketState::created}); !status.ok())
    {
        return status;
    }
    return fd;
}

Socket *SocketTable::find(i32 fd)
{
    for (auto &socket_entry : sockets_)
    {
        if (socket_entry.fd == fd)
        {
            return &socket_entry;
        }
    }
    return nullptr;
}

const Socket *SocketTable::find(i32 fd) const
{
    for (const auto &socket_entry : sockets_)
    {
        if (socket_entry.fd == fd)
        {
            return &socket_entry;
        }
    }
    return nullptr;
}

Status SocketTable::bind(i32 fd, UdpEndpoint local)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    socket_entry->local = local;
    socket_entry->state = SocketState::bound;
    return Status::success();
}

Status SocketTable::listen(i32 fd)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->type != SocketType::tcp || socket_entry->local.port == 0)
    {
        return Status::invalid_argument("TCP socket must be bound before listen");
    }
    if (auto status = stack_->listen_tcp(socket_entry->local.port); !status.ok())
    {
        return status;
    }
    socket_entry->state = SocketState::listening;
    return Status::success();
}

Result<i32> SocketTable::accept(i32 fd)
{
    const auto *listener = find(fd);
    if (listener == nullptr || listener->state != SocketState::listening)
    {
        return Status::invalid_argument("socket is not listening");
    }
    auto accepted = socket(SocketType::tcp);
    if (!accepted)
    {
        return accepted.status();
    }
    auto *socket_entry = find(accepted.value());
    socket_entry->local = listener->local;
    socket_entry->state = SocketState::connected;
    return accepted.value();
}

Status SocketTable::connect(i32 fd, UdpEndpoint remote)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->type == SocketType::tcp)
    {
        auto connection = stack_->connect_tcp(remote, socket_entry->local.port == 0 ? 49152 : socket_entry->local.port);
        if (!connection)
        {
            return connection.status();
        }
        socket_entry->local = connection.value().local;
    }
    socket_entry->remote = remote;
    socket_entry->state = SocketState::connected;
    return Status::success();
}

Result<usize> SocketTable::sendto(i32 fd, std::span<const std::byte> payload, UdpEndpoint remote)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->type != SocketType::udp)
    {
        return Status::invalid_argument("sendto currently supports UDP sockets");
    }
    const auto local = socket_entry->local.port == 0
                           ? UdpEndpoint{.address = stack_->local_address(), .port = 49152}
                           : socket_entry->local;
    if (auto status = stack_->send_udp(local, remote, payload); !status.ok())
    {
        return status;
    }
    return payload.size();
}

Result<UdpDatagram> SocketTable::recvfrom(i32 fd)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->type != SocketType::udp)
    {
        return Status::invalid_argument("recvfrom currently supports UDP sockets");
    }
    return stack_->receive_udp();
}

Result<usize> SocketTable::write(i32 fd, std::span<const std::byte> payload)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->state != SocketState::connected)
    {
        return Status::invalid_argument("socket is not connected");
    }
    if (socket_entry->type != SocketType::udp)
    {
        return Status::unsupported("stream socket write path is not implemented");
    }
    return sendto(fd, payload, socket_entry->remote);
}

Result<usize> SocketTable::read(i32 fd, std::span<std::byte> out)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    if (socket_entry->type != SocketType::udp)
    {
        return Status::unsupported("stream socket read path is not implemented");
    }
    auto datagram = recvfrom(fd);
    if (!datagram)
    {
        return datagram.status();
    }
    const auto count = datagram.value().payload_size < out.size() ? datagram.value().payload_size : out.size();
    for (usize i = 0; i < count; ++i)
    {
        out[i] = datagram.value().payload[i];
    }
    return count;
}

Result<u32> SocketTable::poll(i32 fd) const
{
    const auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    constexpr u32 readable = 0x01;
    constexpr u32 writable = 0x04;
    return static_cast<u32>((socket_entry->type == SocketType::udp && stack_->udp_queued() > 0 ? readable : 0u) |
                            writable);
}

Status SocketTable::shutdown(i32 fd)
{
    auto *socket_entry = find(fd);
    if (socket_entry == nullptr)
    {
        return Status::not_found("socket not found");
    }
    socket_entry->state = SocketState::closed;
    return Status::success();
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
