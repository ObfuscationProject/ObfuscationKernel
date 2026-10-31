#pragma once

#include "ok/net/wire.hpp"
#include "ok/net/interface.hpp"
#include "ok/net/native_udp_readiness_sequence.hpp"
#include <array>
#include <span>

namespace ok::net
{

// Serialized boot-CPU diagnostic profile. The driver owns complete frame
// buffers; this service owns datagrams and never retains borrowed frame bytes.
struct NativeUdpLink
{
    wire::Mac local_mac{}, peer_mac{};
    wire::Ipv4 local_ip{}, peer_ip{};
};

// `unbound`: a valid datagram no socket accepts (the caller may answer with
// ICMP port unreachable).
enum class NativeUdpFrameKind : u8 { dropped, arp_reply, udp_queued, unbound };
struct NativeUdpFrame
{
    NativeUdpFrameKind kind{NativeUdpFrameKind::dropped};
    usize reply_length{};
    // Exact generation-tagged endpoint, only after a datagram is queued.
    u32 queued_handle{};
};

struct NativeUdpReadiness
{
    usize queued_datagrams{};
    u64 sequence{};
    bool terminal{};
    [[nodiscard]] bool readable() const { return queued_datagrams != 0; }
    [[nodiscard]] bool may_wait(u64 observed) const
    {
        return !readable() && !terminal && sequence == observed;
    }
};

// Copied diagnostic state only; callers retain endpoint lifetime separately.
struct NativeUdpBindingSnapshot
{
    bool bound{};
    wire::Ipv4 address{};
    u16 port{};
    [[nodiscard]] bool operator==(const NativeUdpBindingSnapshot &) const = default;
};

struct NativeUdpPeerSnapshot
{
    bool associated{};
    wire::Ipv4 address{};
    u16 port{};
    [[nodiscard]] bool operator==(const NativeUdpPeerSnapshot &) const = default;
};

struct NativeUdpDatagram
{
    wire::Ipv4 source{};
    u16 source_port{};
    wire::Bytes payload{};
};

class NativeUdpService final
{
  public:
    static constexpr usize socket_capacity = 16;
    static constexpr usize queue_capacity = 2;
    static constexpr usize datagram_capacity = 1472;
    static constexpr usize frame_capacity = 1514;

    NativeUdpService() = default;
    explicit NativeUdpService(NativeUdpReadinessCapacity capacity)
        : readiness_sequence_{capacity} {}
    NativeUdpService(const NativeUdpService &) = delete;
    NativeUdpService &operator=(const NativeUdpService &) = delete;
    NativeUdpService(NativeUdpService &&) = delete;
    NativeUdpService &operator=(NativeUdpService &&) = delete;

    // With an attached interface, configure() also configures it from the
    // link: the local address as a /24, the peer as default gateway and
    // permanent neighbour, plus IPv6 link-local autoconfiguration. Routing
    // and neighbour resolution then replace the static peer checks; without
    // one the service keeps the static peer profile.
    Status attach_interface(EthernetInterface &interface);
    [[nodiscard]] EthernetInterface *interface() const { return interface_; }
    Status configure(NativeUdpLink link);
    [[nodiscard]] bool reachable(wire::Ipv4 destination) const;
    [[nodiscard]] bool local_address(wire::Ipv4 address) const;
    Result<u32> open();
    Status close(u32 handle);
    Status bind(u32 handle, wire::Ipv4 address, u16 port);
    // Explicit canonical reservation; existing bind/frame ordering is unchanged.
    Status bind_ephemeral(u32 handle, wire::Ipv4 address);
    // Implicit reservation preserves an explicit address retained by disconnect.
    Status autobind(u32 handle);
    Status connect(u32 handle, wire::Ipv4 address, u16 port);
    Status disconnect(u32 handle);
    // Returns the frame length to transmit, or 0 when the attached interface
    // holds the packet for neighbour resolution (it is sent from there).
    Result<usize> build_datagram(u32 handle, wire::Ipv4 destination, u16 port,
                                 wire::Bytes payload, std::span<std::byte> frame);
    Result<NativeUdpFrame> ingest(wire::Bytes frame, std::span<std::byte> reply);
    // A complete IPv4 packet the interface accepted for a local address.
    Result<NativeUdpFrame> deliver(wire::Bytes packet);
    // Pure copied receive snapshot, including zero-length datagrams. No device
    // I/O, binding, payload borrow or queue reservation is performed.
    Result<NativeUdpReadiness> readiness(u32 handle) const;
    // Pure generation-checked copied state; available during terminal drainage.
    Result<NativeUdpBindingSnapshot> binding(u32 handle) const;
    Result<NativeUdpPeerSnapshot> peer(u32 handle) const;
    Result<NativeUdpDatagram> peek(u32 handle) const;
    Status consume(u32 handle);
    [[nodiscard]] usize active_sockets() const;
    [[nodiscard]] u64 dropped_frames() const { return dropped_frames_; }
    [[nodiscard]] const NativeUdpLink &link() const { return link_; }
    [[nodiscard]] bool configured() const { return configured_; }
    [[nodiscard]] Status status() const
    {
        return readiness_sequence_.terminal()
            ? Status::io_error("native UDP receive readiness is exhausted") : Status::success();
    }

  private:
    struct Datagram
    {
        wire::Ipv4 source{};
        u16 source_port{};
        usize size{};
        std::array<std::byte, datagram_capacity> bytes{};
    };
    struct Socket
    {
        bool used{}, bound{}, retired{}, associated{}, address_locked{}, port_locked{};
        u32 generation{};
        u16 port{};
        wire::Ipv4 address{};
        wire::Ipv4 peer_address{};
        u16 peer_port{};
        usize head{}, count{};
        std::array<Datagram, queue_capacity> queue{};
    };
    [[nodiscard]] Socket *find(u32 handle);
    [[nodiscard]] const Socket *find(u32 handle) const;
    [[nodiscard]] bool port_used(u16 port) const;
    [[nodiscard]] Result<u16> ephemeral_port();
    NativeUdpFrame drop();
    Result<NativeUdpFrame> queue(const wire::Ipv4Packet &ip, const wire::Udp &udp);

    std::array<Socket, socket_capacity> sockets_{};
    NativeUdpLink link_{};
    EthernetInterface *interface_{};
    bool configured_{};
    u16 next_identifier_{0x9000};
    u16 next_ephemeral_{49152};
    u64 dropped_frames_{};
    NativeUdpReadinessSequence<> readiness_sequence_{};
};

} // namespace ok::net
