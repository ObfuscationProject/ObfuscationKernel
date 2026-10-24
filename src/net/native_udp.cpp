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

Status NativeUdpService::configure(NativeUdpLink link)
{
    if (auto state = status(); !state) return state;
    if (configured_) return Status::already_exists("UDP link is already configured");
    if (zero(link.local_mac) || zero(link.peer_mac) || (link.local_mac[0] & 1) ||
        (link.peer_mac[0] & 1) || link.local_mac == link.peer_mac ||
        zero(link.local_ip) || zero(link.peer_ip) || link.local_ip == link.peer_ip)
        return Status::invalid_argument("static UDP link requires distinct unicast identities");
    link_ = link;
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

Result<u32> NativeUdpService::open()
{
    if (auto state = status(); !state) return state;
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
        socket.generation = generation;
        return (generation << 8) | static_cast<u32>(i + 1);
    }
    return Status::no_memory("bounded UDP socket table is full");
}
Status NativeUdpService::close(u32 handle)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    const u32 generation = socket->generation;
    *socket = {};
    socket->generation = generation;
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
Status NativeUdpService::bind(u32 handle, wire::Ipv4 address, u16 port)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (port < 1024 || (address != link_.local_ip && !zero(address)))
        return Status::invalid_argument("unsupported local UDP address or port");
    if (socket->bound || port_used(port)) return Status::already_exists("UDP port is already bound");
    socket->address = address;
    socket->port = port;
    socket->bound = true;
    socket->port_locked = true;
    if (!zero(address)) socket->address_locked = true;
    return Status::success();
}
Status NativeUdpService::bind_ephemeral(u32 handle, wire::Ipv4 address)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (address != link_.local_ip && !zero(address))
        return Status::invalid_argument("unsupported local UDP address");
    if (socket->bound) return Status::already_exists("UDP endpoint is already bound");
    const auto port = ephemeral_port();
    if (!port) return port.status();
    socket->address = address;
    socket->port = port.value();
    socket->bound = true;
    if (!zero(address)) socket->address_locked = true;
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
Status NativeUdpService::connect(u32 handle, wire::Ipv4 address, u16 port)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (address != link_.peer_ip)
        return Status::invalid_argument("UDP peer is outside the static peer profile");
    if (auto state = autobind(handle); !state) return state;
    if (zero(socket->address)) socket->address = link_.local_ip;
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
    socket->peer_address = {};
    socket->peer_port = 0;
    if (!socket->address_locked) socket->address = {};
    if (!socket->port_locked)
    {
        socket->bound = false;
        socket->port = 0;
    }
    return Status::success();
}
Result<usize> NativeUdpService::build_datagram(u32 handle, wire::Ipv4 destination, u16 port,
                                                wire::Bytes payload, std::span<std::byte> frame)
{
    auto *socket = find(handle);
    if (!socket) return Status::bad_descriptor("stale UDP socket handle");
    if (auto state = status(); !state) return state;
    if (!configured_) return Status::not_initialized("UDP link is not configured");
    if (destination != link_.peer_ip)
        return Status::invalid_argument("UDP destination is outside the static peer profile");
    if (payload.size() > datagram_capacity)
        return Status::message_size("UDP datagram exceeds the unfragmented IPv4 MTU");
    const usize packet_size = 28 + payload.size();
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
    const auto built = wire::build_udp(frame.subspan(14, packet_size), link_.local_ip,
        destination, source_port, port, next_identifier_, 64, payload);
    if (!built) return built.status();
    if (!socket->bound)
    {
        socket->address = link_.local_ip;
        socket->port = source_port;
        socket->bound = true;
    }
    ++next_identifier_;
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
    for (usize index = 0; index < sockets_.size(); ++index)
    {
        auto &socket = sockets_[index];
        if (socket.used && socket.bound && socket.port == udp.value().destination_port)
        {
            if ((socket.address != wire::Ipv4{} && socket.address != ip.value().destination) ||
                (socket.associated && (socket.peer_address != ip.value().source ||
                 (socket.peer_port && socket.peer_port != udp.value().source_port)))) return drop();
            if (socket.count == queue_capacity) return drop();
            if (!readiness_sequence_.advance()) return status();
            auto &queued = socket.queue[(socket.head + socket.count) % queue_capacity];
            queued.source = ip.value().source;
            queued.source_port = udp.value().source_port;
            queued.size = udp.value().payload.size();
            for (usize i = 0; i < queued.size; ++i) queued.bytes[i] = udp.value().payload[i];
            ++socket.count;
            return NativeUdpFrame{NativeUdpFrameKind::udp_queued, 0,
                (socket.generation << 8) | static_cast<u32>(index + 1)};
        }
    }
    return drop();
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
