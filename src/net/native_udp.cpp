#include "ok/net/native_udp.hpp"

namespace ok::net
{
namespace
{
bool zero(wire::Mac address)
{
    for (u8 byte : address) if (byte) return false;
    return true;
}
bool zero(wire::Ipv4 address)
{
    for (u8 byte : address) if (byte) return false;
    return true;
}
IpAddress unspecified(Family family)
{
    return family == Family::ipv6 ? IpAddress{wire::Ipv6{}} : IpAddress{};
}
bool broadcast(wire::Mac address)
{
    for (u8 byte : address) if (byte != 0xff) return false;
    return true;
}
bool overlap(wire::Bytes input, std::span<std::byte> output, usize written)
{
    if (input.empty() || !written) return false;
    const auto first = reinterpret_cast<uptr>(input.data());
    const auto second = reinterpret_cast<uptr>(output.data());
    return first >= second ? first - second < written : second - first < input.size();
}
void put_mac(std::span<std::byte> output, usize offset, wire::Mac address)
{
    for (usize i = 0; i < address.size(); ++i) output[offset + i] = std::byte{address[i]};
}
void put_ip(std::span<std::byte> output, usize offset, wire::Ipv4 address)
{
    for (usize i = 0; i < address.size(); ++i) output[offset + i] = std::byte{address[i]};
}
}

Status NativeUdpService::attach_interface(EthernetInterface &interface)
{
    if (configured_ || interface_) return Status::busy("UDP interface must attach before configuration");
    interface_ = &interface;
    return Status::success();
}
bool NativeUdpService::reachable(const IpAddress &destination) const
{
    if (!interface_) return destination == link_.peer_ip;
    return destination.v4() ? interface_->routable(destination.ipv4()) : interface_->routable(destination.ipv6());
}
bool NativeUdpService::local_address(const IpAddress &address) const
{
    if (!interface_) return address == link_.local_ip;
    return address.v4() ? interface_->local(address.ipv4()) : interface_->local(address.ipv6());
}
Status NativeUdpService::configure(NativeUdpLink link)
{
    if (auto state = status(); !state) return state;
    if (configured_) return Status::already_exists("UDP link is already configured");
    if (zero(link.local_mac) || zero(link.peer_mac) || (link.local_mac[0] & 1) ||
        (link.peer_mac[0] & 1) || link.local_mac == link.peer_mac ||
        zero(link.local_ip) || zero(link.peer_ip) || link.local_ip == link.peer_ip)
        return Status::invalid_argument("static UDP link requires distinct unicast identities");
    if (interface_ && !interface_->configured())
    {
        const InterfaceConfig config{link.local_mac, 1500, Ipv4Configuration{link.local_ip, 24, link.peer_ip, link.peer_mac}, true};
        if (auto status = interface_->configure(config); !status) return status;
    }
    link_ = link;
    configured_ = true;
    return Status::success();
}

Status NativeUdpService::configure(const InterfaceConfig &config)
{
    if (auto state = status(); !state) return state;
    if (configured_) return Status::already_exists("UDP link is already configured");
    if (!interface_) return Status::not_initialized("UDP interface configuration needs an attached interface");
    if (!config.ipv4) return Status::invalid_argument("UDP interface configuration needs an IPv4 address");
    if (!interface_->configured())
        if (auto status = interface_->configure(config); !status) return status;
    link_ = {};
    link_.local_mac = config.mac; link_.local_ip = config.ipv4->address;
    configured_ = true;
    return Status::success();
}

NativeUdpService::Socket *NativeUdpService::find(u32 handle)
{
    const u32 index = handle & 0xffu;
    if (!index || index > sockets_.size()) return nullptr;
    auto &socket = sockets_[index - 1];
    return socket.used && socket.generation == (handle >> 8) ? &socket : nullptr;
}
const NativeUdpService::Socket *NativeUdpService::find(u32 handle) const
{
    return const_cast<NativeUdpService *>(this)->find(handle);
}

Result<u32> NativeUdpService::open(Family family)
{
    if (auto state = status(); !state) return state;
    if (family == Family::ipv6 && !interface_) return Status::unsupported("IPv6 UDP needs the network interface");
    for (usize i = 0; i < sockets_.size(); ++i)
    {
        auto &socket = sockets_[i];
        if (socket.used || socket.retired) continue;
        if (socket.generation == 0x00ffffffu)
        {
            socket.retired = true;
            continue;
        }
        const u32 generation = socket.generation + 1;
        socket = {};
        socket.used = true;
        socket.family = family;
        socket.address = socket.peer_address = unspecified(family);
        socket.generation = generation;
        return (generation << 8) | static_cast<u32>(i + 1);
    }
    return Status::no_memory("bounded UDP socket table is full");
}
Result<Family> NativeUdpService::family(u32 handle) const
{
    const auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    return socket->family;
}
Status NativeUdpService::close(u32 handle)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    const u32 generation = socket->generation;
    *socket = {};
    socket->generation = generation;
    socket->changed = true;
    return Status::success();
}
bool NativeUdpService::port_used(u16 port) const
{
    for (const auto &socket : sockets_)
        if (socket.used && socket.bound && socket.port == port) return true;
    return false;
}
Result<u16> NativeUdpService::ephemeral_port()
{
    // There are at most sixteen sockets, so seventeen consecutive candidates
    // guarantee a free port without scanning the entire ephemeral range.
    for (usize attempt = 0; attempt <= socket_capacity; ++attempt)
    {
        const u16 port = next_ephemeral_;
        next_ephemeral_ = port == 65535 ? 49152 : static_cast<u16>(port + 1);
        if (!port_used(port)) return port;
    }
    return Status::no_space("no ephemeral UDP port in the bounded probe window");
}
Status NativeUdpService::bind(u32 handle, IpAddress address, u16 port)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (address.family != socket->family) return Status::invalid_argument("UDP address family differs from the socket");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (port < 1024 || (!local_address(address) && !address.unspecified()))
        return Status::invalid_argument("unsupported local UDP address or port");
    if (socket->bound || port_used(port)) return Status::already_exists("UDP port is already bound");
    socket->address = address;
    socket->port = port;
    socket->bound = true;
    socket->port_locked = true;
    if (!address.unspecified()) socket->address_locked = true;
    return Status::success();
}
Status NativeUdpService::bind_ephemeral(u32 handle, IpAddress address)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (address.family != socket->family) return Status::invalid_argument("UDP address family differs from the socket");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (!local_address(address) && !address.unspecified())
        return Status::invalid_argument("unsupported local UDP address");
    if (socket->bound) return Status::already_exists("UDP endpoint is already bound");
    const auto port = ephemeral_port();
    if (!port) return port.status();
    socket->address = address;
    socket->port = port.value();
    socket->bound = true;
    if (!address.unspecified()) socket->address_locked = true;
    return Status::success();
}
Status NativeUdpService::autobind(u32 handle)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (socket->bound) return Status::success();
    const auto port = ephemeral_port();
    if (!port) return port.status();
    socket->port = port.value();
    socket->bound = true;
    return Status::success();
}
Status NativeUdpService::connect(u32 handle, IpAddress address, u16 port)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (address.family != socket->family) return Status::invalid_argument("UDP address family differs from the socket");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (!reachable(address))
        return Status::invalid_argument("UDP peer has no route");
    if (auto state = autobind(handle); !state) return state;
    if (socket->address.unspecified())
    {
        auto source = local_source(*socket, address);
        if (!source) return source.status();
        socket->address = source.value();
    }
    socket->peer_address = address;
    socket->peer_port = port;
    socket->associated = true;
    return Status::success();
}
Status NativeUdpService::disconnect(u32 handle)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    socket->associated = false;
    socket->peer_address = unspecified(socket->family);
    socket->peer_port = 0;
    if (!socket->address_locked) socket->address = unspecified(socket->family);
    if (!socket->port_locked)
    {
        socket->bound = false;
        socket->port = 0;
    }
    return Status::success();
}
Result<IpAddress> NativeUdpService::local_source(const Socket &socket, const IpAddress &destination) const
{
    if (socket.family == Family::ipv4 || !interface_) return IpAddress{link_.local_ip};
    if (!socket.address.unspecified()) return socket.address;
    auto source = interface_->source(destination.ipv6());
    if (!source) return source.status();
    return IpAddress{source.value()};
}
Result<usize> NativeUdpService::build_datagram(u32 handle, IpAddress destination, u16 port,
                                                wire::Bytes payload, std::span<std::byte> frame)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (destination.family != socket->family) return Status::invalid_argument("UDP address family differs from the socket");
    if (!reachable(destination))
        return Status::invalid_argument("UDP destination has no route");
    const bool v6 = socket->family == Family::ipv6;
    const usize header = v6 ? 48 : 28;
    if (payload.size() > datagram_capacity || header + payload.size() > 1500)
        return Status::message_size("UDP datagram exceeds the unfragmented MTU");
    const usize packet_size = header + payload.size();
    const usize frame_size = 14 + packet_size > 60 ? 14 + packet_size : 60;
    if (frame.size() < frame_size) return Status::overflow("UDP frame output capacity is too small");
    if (overlap(payload, frame, frame_size)) return Status::invalid_argument("UDP payload aliases the frame output");
    u16 source_port = socket->port;
    if (!socket->bound)
    {
        auto available = ephemeral_port();
        if (!available) return available.status();
        source_port = available.value();
    }
    auto source = local_source(*socket, destination);
    if (!source) return source.status();
    const auto built = v6
        ? wire::build_udp<wire::Ipv6Family>(frame.subspan(14, packet_size), source.value().ipv6(), destination.ipv6(),
                                            source_port, port, 0, 64, payload)
        : wire::build_udp(frame.subspan(14, packet_size), link_.local_ip, destination.ipv4(), source_port, port,
                          next_identifier_, 64, payload);
    if (!built) return built.status();
    if (!socket->bound)
    {
        if (!v6) socket->address = link_.local_ip;
        socket->port = source_port;
        socket->bound = true;
    }
    if (!v6) ++next_identifier_;
    if (interface_) return interface_->output(frame, packet_size);
    put_mac(frame, 0, link_.peer_mac);
    put_mac(frame, 6, link_.local_mac);
    wire::be16(frame, 12, 0x0800);
    for (usize i = 14 + packet_size; i < frame_size; ++i) frame[i] = std::byte{};
    return frame_size;
}
NativeUdpFrame NativeUdpService::drop()
{
    if (dropped_frames_ != ~u64{0}) ++dropped_frames_;
    return {};
}
Result<NativeUdpFrame> NativeUdpService::ingest(wire::Bytes frame, std::span<std::byte> reply)
{
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    const auto ethernet = wire::ethernet(frame);
    if (!ethernet || ethernet.value().source != link_.peer_mac) return drop();
    if (ethernet.value().type == 0x0806)
    {
        if (ethernet.value().destination != link_.local_mac && !broadcast(ethernet.value().destination))
            return drop();
        const auto arp = wire::arp(ethernet.value().payload);
        if (!arp || arp.value().operation != 1 || arp.value().sender_mac != link_.peer_mac ||
            arp.value().sender_ip != link_.peer_ip || arp.value().target_ip != link_.local_ip ||
            (!zero(arp.value().target_mac) && arp.value().target_mac != link_.local_mac))
            return drop();
        if (reply.size() < 60) return Status::overflow("ARP reply frame capacity is too small");
        for (usize i = 0; i < 60; ++i) reply[i] = std::byte{};
        put_mac(reply, 0, link_.peer_mac); put_mac(reply, 6, link_.local_mac);
        wire::be16(reply, 12, 0x0806); wire::be16(reply, 14, 1); wire::be16(reply, 16, 0x0800);
        reply[18] = std::byte{6}; reply[19] = std::byte{4}; wire::be16(reply, 20, 2);
        put_mac(reply, 22, link_.local_mac); put_ip(reply, 28, link_.local_ip);
        put_mac(reply, 32, link_.peer_mac); put_ip(reply, 38, link_.peer_ip);
        return NativeUdpFrame{NativeUdpFrameKind::arp_reply, 60};
    }
    if (ethernet.value().type != 0x0800 || ethernet.value().destination != link_.local_mac)
        return drop();
    const auto ip = wire::ipv4(ethernet.value().payload);
    if (!ip || ip.value().source != link_.peer_ip || ip.value().destination != link_.local_ip ||
        !ip.value().ttl)
        return drop();
    const auto udp = wire::udp(ip.value());
    if (!udp || !udp.value().checksum_present || udp.value().payload.size() > datagram_capacity ||
        wire::be16(ip.value().payload, 4) != ip.value().payload.size())
        return drop();
    auto queued = queue(ip.value().source, ip.value().destination, udp.value());
    if (queued && queued.value().kind == NativeUdpFrameKind::unbound) return drop();
    return queued;
}
Result<NativeUdpFrame> NativeUdpService::deliver(wire::Bytes packet)
{
    if (auto state = status(); !state) return state;
    if (!configured_ || !interface_) return Status::not_initialized("UDP interface is not configured");
    IpAddress source{}, destination{};
    Result<wire::Udp> udp{Status::invalid_argument()};
    wire::Bytes transport{};
    if (wire::ip_version(packet) == 6)
    {
        const auto ip = wire::ipv6(packet);
        if (!ip || !ip.value().hop_limit) return drop();
        source = ip.value().source; destination = ip.value().destination; transport = ip.value().payload;
        udp = wire::udp<wire::Ipv6Family>(ip.value());
    }
    else
    {
        const auto ip = wire::ipv4(packet);
        if (!ip || !ip.value().ttl) return drop();
        source = ip.value().source; destination = ip.value().destination; transport = ip.value().payload;
        udp = wire::udp(ip.value());
    }
    // The receive queue keeps the static profile's requirements: a checksum
    // and no trailing bytes after the datagram.
    if (!udp || !udp.value().checksum_present || udp.value().payload.size() > datagram_capacity ||
        wire::be16(transport, 4) != transport.size())
        return drop();
    auto queued = queue(source, destination, udp.value());
    if (queued && queued.value().kind == NativeUdpFrameKind::unbound && dropped_frames_ != ~u64{0}) ++dropped_frames_;
    return queued;
}
Result<NativeUdpFrame> NativeUdpService::queue(const IpAddress &source, const IpAddress &destination, const wire::Udp &udp)
{
    for (usize index = 0; index < sockets_.size(); ++index)
    {
        auto &socket = sockets_[index];
        if (socket.used && socket.bound && socket.family == destination.family && socket.port == udp.destination_port)
        {
            if ((!socket.address.unspecified() && socket.address != destination) ||
                (socket.associated && (socket.peer_address != source ||
                 (socket.peer_port && socket.peer_port != udp.source_port)))) return drop();
            if (socket.count == queue_capacity) return drop();
            if (!readiness_sequence_.advance()) return status();
            auto &queued = socket.queue[(socket.head + socket.count) % queue_capacity];
            queued.source = source;
            queued.source_port = udp.source_port;
            queued.size = udp.payload.size();
            for (usize i = 0; i < queued.size; ++i) queued.bytes[i] = udp.payload[i];
            ++socket.count; socket.changed = true;
            return NativeUdpFrame{NativeUdpFrameKind::udp_queued, 0,
                (socket.generation << 8) | static_cast<u32>(index + 1)};
        }
    }
    // The caller decides whether an unbound datagram counts as dropped.
    return NativeUdpFrame{NativeUdpFrameKind::unbound};
}
Result<NativeUdpReadiness> NativeUdpService::readiness(u32 handle) const
{
    const auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    return NativeUdpReadiness{socket->count, readiness_sequence_.value(), readiness_sequence_.terminal()};
}
Result<NativeUdpBindingSnapshot> NativeUdpService::binding(u32 handle) const
{
    const auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    return NativeUdpBindingSnapshot{socket->bound, socket->address, socket->port};
}
Result<NativeUdpPeerSnapshot> NativeUdpService::peer(u32 handle) const
{
    const auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    return NativeUdpPeerSnapshot{socket->associated, socket->peer_address, socket->peer_port};
}
Result<NativeUdpDatagram> NativeUdpService::peek(u32 handle) const
{
    const auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (!socket->count)
        return status().ok() ? Status::would_block("UDP receive queue is empty") : status();
    const auto &message = socket->queue[socket->head];
    return NativeUdpDatagram{message.source, message.source_port, {message.bytes.data(), message.size}};
}
Status NativeUdpService::consume(u32 handle)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (!socket->count)
        return status().ok() ? Status::would_block("UDP receive queue is empty") : status();
    // Exhaustion retires admission, but consuming retained data must succeed.
    static_cast<void>(readiness_sequence_.advance());
    socket->queue[socket->head].size = 0;
    socket->head = (socket->head + 1) % queue_capacity;
    --socket->count;
    return Status::success();
}
usize NativeUdpService::active_sockets() const
{
    usize count = 0;
    for (const auto &socket : sockets_) if (socket.used) ++count;
    return count;
}
} // namespace ok::net
