#pragma once

#include <ok/net/icmp.hpp>
#include <ok/net/neighbour.hpp>
#include <ok/net/route.hpp>

namespace ok::net
{
struct Ipv4Configuration
{
    wire::Ipv4 address{};
    u8 prefix{24};
    std::optional<wire::Ipv4> gateway{};
    // A statically known gateway link address becomes a permanent neighbour.
    std::optional<wire::Mac> gateway_mac{};
};

struct InterfaceConfig
{
    wire::Mac mac{};
    u16 mtu{1500};
    std::optional<Ipv4Configuration> ipv4{};
    // Link-local address, duplicate address detection and SLAAC.
    bool ipv6{true};
};

enum class Ipv6AddressState : u8 { tentative, preferred, deprecated, duplicated };
struct Ipv6AddressSnapshot
{
    wire::Ipv6 address{};
    u8 prefix{};
    Ipv6AddressState state{};
    u64 valid_until{}, preferred_until{};
};

// `none`: filtered or dropped; `control`: answered or learned by the
// interface itself (ARP, neighbour discovery, echo).
enum class Delivery : u8 { none, control, ipv4, ipv6 };
// A packet for an upper layer: `packet` is the complete network-layer packet
// borrowed from the received frame. `multicast` records a link-layer or IP
// group/broadcast destination, which never elicits an ICMP error.
struct Received
{
    Delivery family{};
    u8 protocol{};
    wire::Bytes packet{};
    bool multicast{};
};

struct EchoReply
{
    u8 version{};
    wire::Ipv6 source{};
    u16 identifier{}, sequence{};
    usize size{};
    u64 at{};
};

struct InterfaceStats
{
    u64 received{}, filtered{}, dropped{}, delivered{};
    u64 arp_requests{}, arp_replies{}, arp_conflicts{};
    u64 echo_requests{}, echo_replies{}, icmp_errors{}, icmp_rate_limited{};
    u64 solicitations{}, advertisements{}, router_advertisements{}, dad_conflicts{};
    u64 no_route{}, held{}, control_sent{}, control_dropped{};
};

// One Ethernet interface: link filter, ARP and neighbour discovery, ICMP
// echo and errors, IPv6 address configuration (RFC 4862), and routed egress.
// The owner serializes every call; received frames are borrowed for the call
// and returned packet views borrow them. Frames this interface originates
// (replies, solicitations, held packets) leave through control()/flush().
class EthernetInterface final
{
  public:
    static constexpr usize frame_capacity = 1514, minimum_frame = 60, control_capacity = 4;
    static constexpr usize max_ipv6_addresses = 3;
    static constexpr u64 dad_retransmit_ms = 1000, router_solicitation_interval_ms = 4000;
    static constexpr u8 router_solicitations = 3, default_hop_limit = 64;
    static constexpr u64 icmp_token_ms = 100;
    static constexpr u8 icmp_burst = 10;
    using Arp = NeighbourCache<wire::Ipv4, 16, 4, 1500>;
    using Nd = NeighbourCache<wire::Ipv6, 16, 4, 1500>;

    EthernetInterface() = default;
    EthernetInterface(const EthernetInterface &) = delete;
    EthernetInterface &operator=(const EthernetInterface &) = delete;

    [[nodiscard]] bool configured() const { return configured_; }
    [[nodiscard]] const InterfaceConfig &config() const { return config_; }
    [[nodiscard]] const InterfaceStats &stats() const { return stats_; }
    [[nodiscard]] const Arp &arp() const { return arp_; }
    [[nodiscard]] const Nd &neighbours() const { return nd_; }
    [[nodiscard]] const RouteTable<wire::Ipv4> &routes4() const { return routes4_; }
    [[nodiscard]] const RouteTable<wire::Ipv6> &routes6() const { return routes6_; }
    [[nodiscard]] u16 ipv6_mtu() const { return mtu6_; }
    [[nodiscard]] std::optional<EchoReply> last_echo_reply() const { return last_echo_; }
    [[nodiscard]] u64 now() const { return now_; }
    template <class Visit> void each_ipv6(Visit &&visit) const
    { for (const auto &a : addresses_) if (a.used) visit(Ipv6AddressSnapshot{a.address, a.prefix, a.state, a.valid_until, a.preferred_until}); }

    Status configure(const InterfaceConfig &config)
    {
        if (configured_) return Status::already_exists("network interface is already configured");
        if (wire::multicast(config.mac) || config.mac == wire::Mac{}) return Status::invalid_argument("interface link address is not unicast");
        if (config.mtu < 576 || config.mtu > 1500) return Status::invalid_argument("interface MTU is outside the Ethernet profile");
        if (config.ipv4)
        {
            const auto &v4 = *config.ipv4;
            if (v4.prefix == 0 || v4.prefix > 30 || !unicast(v4.address)) return Status::invalid_argument("invalid IPv4 interface address");
            if (v4.gateway && (!unicast(*v4.gateway) || *v4.gateway == v4.address ||
                               !wire::prefix_match(*v4.gateway, v4.address, v4.prefix)))
                return Status::invalid_argument("IPv4 gateway is not an on-link unicast neighbour");
            if (auto s = routes4_.add({v4.address, v4.prefix, {}, 0, 0, RouteOrigin::connected}); !s) return s;
            if (v4.gateway)
            {
                if (auto s = routes4_.add({{}, 0, *v4.gateway, 0, 100, RouteOrigin::configured}); !s) return s;
                if (v4.gateway_mac) if (auto s = arp_.add_permanent(*v4.gateway, *v4.gateway_mac, now_); !s) return s;
            }
        }
        if (config.ipv6)
        {
            if (auto s = routes6_.add({{0xfe, 0x80}, 64, {}, 0, 256, RouteOrigin::connected}); !s) return s;
            add_address(wire::link_local_from_mac(config.mac), 64, ~u64{0}, ~u64{0});
        }
        config_ = config; mtu6_ = config.mtu < 1280 ? 0 : config.mtu; configured_ = true;
        return Status::success();
    }

    [[nodiscard]] bool local(const wire::Ipv4 &address) const { return config_.ipv4 && config_.ipv4->address == address; }
    [[nodiscard]] bool local(const wire::Ipv6 &address) const
    {
        const auto *slot = find_address(address);
        return slot && slot->state != Ipv6AddressState::tentative && slot->state != Ipv6AddressState::duplicated;
    }
    [[nodiscard]] bool routable(const wire::Ipv4 &destination) const
    { return configured_ && unicast(destination) && !local(destination) && routes4_.lookup(destination, now_); }
    [[nodiscard]] bool routable(const wire::Ipv6 &destination) const
    {
        return configured_ && !wire::multicast(destination) && !wire::unspecified(destination) &&
            !wire::loopback(destination) && !local(destination) && routes6_.lookup(destination, now_);
    }
    [[nodiscard]] Result<wire::Ipv4> source(const wire::Ipv4 &) const
    {
        if (!config_.ipv4) return Status::not_found("interface has no IPv4 address");
        return config_.ipv4->address;
    }
    // RFC 6724 reduced to scope: link-local destinations use the link-local
    // address, others a preferred global, then a deprecated one.
    [[nodiscard]] Result<wire::Ipv6> source(const wire::Ipv6 &destination) const
    {
        const bool link = wire::link_local(destination) || (wire::multicast(destination) && (destination[1] & 15u) <= 2);
        const Address *best = nullptr;
        for (const auto &a : addresses_)
        {
            if (!a.used || a.state == Ipv6AddressState::tentative || a.state == Ipv6AddressState::duplicated) continue;
            if (wire::link_local(a.address) != link) continue;
            if (!best || (best->state == Ipv6AddressState::deprecated && a.state == Ipv6AddressState::preferred)) best = &a;
        }
        if (!best && !link) return source(wire::Ipv6{0xfe, 0x80});
        if (!best) return Status::not_found("interface has no usable IPv6 address");
        return best->address;
    }

    // Classifies one received frame. Link control (ARP, neighbour discovery,
    // echo) is answered here; transport packets are returned for delivery.
    Received receive(wire::Bytes frame, u64 now)
    {
        advance(now); add(stats_.received);
        if (!configured_) return filtered();
        auto ethernet = wire::ethernet(frame);
        if (!ethernet) return dropped();
        const auto &eth = ethernet.value();
        if (wire::multicast(eth.source) || eth.source == config_.mac) return dropped();
        const bool group = wire::multicast(eth.destination);
        if (eth.destination != config_.mac && !accept_group(eth.destination)) return filtered();
        if (eth.type == wire::ethertype_arp) { receive_arp(eth); return consumed(); }
        if (eth.type == wire::ethertype_ipv4) return receive_ipv4(eth.payload, group);
        if (eth.type == wire::ethertype_ipv6) return receive_ipv6(eth.payload, group);
        return filtered();
    }

    // An upper layer found no receiver for a delivered packet: answer with
    // port unreachable unless RFC 1122 / RFC 4443 section 2.4(e) forbid it.
    void unreachable(const Received &received)
    {
        if (received.multicast || (received.family != Delivery::ipv4 && received.family != Delivery::ipv6)) return;
        if (received.family == Delivery::ipv4)
        {
            auto ip = wire::ipv4(received.packet); if (!ip) return;
            if (ip.value().protocol == wire::protocol_icmp || !unicast(ip.value().source)) return;
            send_error<wire::Ipv4Family>(ip.value().destination, ip.value().source,
                wire::icmpv4::unreachable, wire::icmpv4::port_unreachable, received.packet);
            return;
        }
        auto ip = wire::ipv6(received.packet); if (!ip) return;
        if (ip.value().protocol == wire::protocol_icmpv6 || wire::unspecified(ip.value().source)) return;
        send_error<wire::Ipv6Family>(ip.value().destination, ip.value().source,
            wire::icmpv6::unreachable, wire::icmpv6::port_unreachable, received.packet);
    }

    // `frame` holds a network-layer packet of `length` bytes at offset 14.
    // Returns the frame length ready to transmit, or 0 when the packet is
    // held for neighbour resolution (or dropped by it, which is counted).
    Result<usize> output(std::span<std::byte> frame, usize length)
    {
        if (!configured_) return Status::not_initialized("network interface is not configured");
        if (frame.size() < minimum_frame || length > frame.size() - 14 || length > config_.mtu)
            return Status::overflow("packet exceeds the interface frame");
        const wire::Bytes packet{frame.data() + 14, length};
        const u8 version = wire::ip_version(packet);
        if (version == 4 && length >= 20)
        {
            wire::Ipv4 destination{};
            for (usize i = 0; i < 4; ++i) destination[i] = std::to_integer<u8>(packet[16 + i]);
            if (wire::broadcast(destination) || (config_.ipv4 && subnet_broadcast(destination)))
                return finish(frame, length, wire::broadcast_mac, wire::ethertype_ipv4);
            if (wire::multicast(destination))
                return finish(frame, length, {0x01, 0x00, 0x5e, static_cast<u8>(destination[1] & 0x7fu), destination[2], destination[3]},
                              wire::ethertype_ipv4);
            return routed<wire::Ipv4>(frame, length, destination, routes4_, arp_, wire::ethertype_ipv4);
        }
        if (version == 6 && length >= 40)
        {
            wire::Ipv6 destination{};
            for (usize i = 0; i < 16; ++i) destination[i] = std::to_integer<u8>(packet[24 + i]);
            if (wire::multicast(destination)) return finish(frame, length, wire::multicast_mac(destination), wire::ethertype_ipv6);
            return routed<wire::Ipv6>(frame, length, destination, routes6_, nd_, wire::ethertype_ipv6);
        }
        return Status::invalid_argument("packet is not IPv4 or IPv6");
    }

    // Diagnostic echo request; the reply is recorded in last_echo_reply().
    template <wire::AddressFamily Family>
    Status ping(const typename Family::Address &destination, u16 identifier, u16 sequence, wire::Bytes payload)
    {
        auto from = source(destination); if (!from) return from.status();
        auto built = wire::build_icmp<Family>(std::span{scratch_}.subspan(14), from.value(), destination, default_hop_limit,
            next_identification(), Family::version == 6 ? wire::icmpv6::echo_request : wire::icmpv4::echo_request,
            0, (u32{identifier} << 16) | sequence, payload);
        if (!built) return built.status();
        return send(built.value());
    }

    // Timers: duplicate address detection, router solicitation, neighbour
    // probes and route/address lifetimes.
    Status tick(u64 now)
    {
        advance(now);
        if (!configured_) return Status::success();
        for (auto &a : addresses_)
        {
            if (!a.used) continue;
            if (now_ >= a.valid_until) { remove_address(a); continue; }
            if (a.state == Ipv6AddressState::preferred && now_ >= a.preferred_until) a.state = Ipv6AddressState::deprecated;
            if (a.state != Ipv6AddressState::tentative || now_ < a.dad_deadline) continue;
            if (a.dad_sent)
            {
                a.state = now_ >= a.preferred_until ? Ipv6AddressState::deprecated : Ipv6AddressState::preferred;
                if (wire::link_local(a.address)) solicit_deadline_ = now_;
                continue;
            }
            auto built = wire::build_neighbor_solicitation(std::span{scratch_}.subspan(14), {}, wire::solicited_node(a.address), a.address, {});
            if (built) static_cast<void>(send(built.value()));
            ++a.dad_sent; a.dad_deadline = deadline(now_, dad_retransmit_ms);
        }
        if (config_.ipv6 && solicitations_ < router_solicitations && now_ >= solicit_deadline_ && !router_seen_)
        {
            auto from = source(wire::all_routers);
            if (from)
            {
                auto built = wire::build_router_solicitation(std::span{scratch_}.subspan(14), from.value(), config_.mac);
                if (built) static_cast<void>(send(built.value()));
                ++solicitations_; solicit_deadline_ = deadline(now_, router_solicitation_interval_ms);
            }
        }
        std::array<Solicitation<wire::Ipv4>, 4> arp{};
        auto due4 = arp_.tick(now_, arp); if (!due4) return due4.status();
        for (usize i = 0; i < due4.value(); ++i) solicit(arp[i]);
        std::array<Solicitation<wire::Ipv6>, 4> nd{};
        auto due6 = nd_.tick(now_, nd); if (!due6) return due6.status();
        for (usize i = 0; i < due6.value(); ++i) solicit(nd[i]);
        routes4_.expire(now_); routes6_.expire(now_);
        return Status::success();
    }
    [[nodiscard]] std::optional<u64> next_deadline() const
    {
        std::optional<u64> next;
        const auto take = [&next](std::optional<u64> value) { if (value && (!next || *value < *next)) next = value; };
        take(arp_.next_deadline()); take(nd_.next_deadline()); take(routes4_.next_expiry()); take(routes6_.next_expiry());
        for (const auto &a : addresses_)
            if (a.used)
            {
                if (a.state == Ipv6AddressState::tentative) take(a.dad_deadline);
                if (a.valid_until != ~u64{0}) take(a.valid_until);
                if (a.state == Ipv6AddressState::preferred && a.preferred_until != ~u64{0}) take(a.preferred_until);
            }
        if (config_.ipv6 && solicitations_ < router_solicitations && !router_seen_ && solicit_deadline_ != ~u64{0})
            take(solicit_deadline_);
        return next;
    }

    // Frames this interface originated, oldest first.
    [[nodiscard]] std::optional<wire::Bytes> control() const
    {
        if (!control_count_) return {};
        const auto &f = control_[control_head_]; return wire::Bytes{f.bytes.data(), f.length};
    }
    [[nodiscard]] usize control_pending() const { return control_count_; }
    void pop_control()
    {
        if (!control_count_) return;
        control_head_ = (control_head_ + 1) % control_capacity; --control_count_; add(stats_.control_sent);
    }
    // Offers packets held for resolution as complete frames; the sink returns
    // false to keep one (no transmit credit) and stop.
    template <class Sink>
        requires requires(Sink &sink, wire::Bytes frame) { { sink(frame) } -> std::same_as<bool>; }
    usize flush(Sink &&sink)
    {
        const auto emit = [this, &sink](const wire::Mac &mac, wire::Bytes packet, u16 type) {
            for (usize i = 0; i < packet.size(); ++i) flush_[14 + i] = packet[i];
            auto length = finish(flush_, packet.size(), mac, type);
            return length && sink(wire::Bytes{flush_.data(), length.value()});
        };
        usize sent = arp_.flush([&](const wire::Mac &mac, const wire::Ipv4 &, wire::Bytes p) { return emit(mac, p, wire::ethertype_ipv4); });
        sent += nd_.flush([&](const wire::Mac &mac, const wire::Ipv6 &, wire::Bytes p) { return emit(mac, p, wire::ethertype_ipv6); });
        return sent;
    }
    // Upper-layer reachability confirmation (TCP forward progress).
    void confirm(const wire::Ipv4 &neighbour) { arp_.confirm(neighbour, now_); }
    void confirm(const wire::Ipv6 &neighbour) { nd_.confirm(neighbour, now_); }

  private:
    struct Address
    {
        bool used{}; wire::Ipv6 address{}; u8 prefix{}; Ipv6AddressState state{};
        u64 valid_until{}, preferred_until{}, dad_deadline{}; u8 dad_sent{};
    };
    struct Frame { usize length{}; std::array<std::byte, frame_capacity> bytes{}; };
    static void add(u64 &value) { if (value != ~u64{0}) ++value; }
    static constexpr u64 deadline(u64 now, u64 delay) { return now > ~u64{0} - delay ? ~u64{0} : now + delay; }
    static bool unicast(const wire::Ipv4 &a)
    { return !wire::unspecified(a) && !wire::multicast(a) && !wire::broadcast(a) && !wire::loopback(a) && a[0] < 240; }
    void advance(u64 now) { if (now > now_) now_ = now; }
    Received filtered() { add(stats_.filtered); return {}; }
    static Received consumed() { return {Delivery::control}; }
    Received dropped() { add(stats_.dropped); return {}; }
    u16 next_identification() { return ++identification_; }
    bool subnet_broadcast(const wire::Ipv4 &a) const
    {
        const auto &v4 = *config_.ipv4;
        if (!wire::prefix_match(a, v4.address, v4.prefix)) return false;
        for (u8 bit = v4.prefix; bit < 32; ++bit) if (!((a[bit / 8] >> (7 - bit % 8)) & 1u)) return false;
        return true;
    }
    bool accept_group(const wire::Mac &mac) const
    {
        if (mac == wire::broadcast_mac) return true;
        if (!config_.ipv6 || mac[0] != 0x33 || mac[1] != 0x33) return false;
        if (mac == wire::multicast_mac(wire::all_nodes)) return true;
        for (const auto &a : addresses_)
            if (a.used && a.state != Ipv6AddressState::duplicated && mac == wire::multicast_mac(wire::solicited_node(a.address))) return true;
        return false;
    }
    const Address *find_address(const wire::Ipv6 &address) const
    { for (const auto &a : addresses_) if (a.used && a.address == address) return &a; return nullptr; }
    Address *find_address(const wire::Ipv6 &address)
    { for (auto &a : addresses_) if (a.used && a.address == address) return &a; return nullptr; }
    bool joined(const wire::Ipv6 &group) const
    {
        if (group == wire::all_nodes) return true;
        for (const auto &a : addresses_)
            if (a.used && a.state != Ipv6AddressState::duplicated && group == wire::solicited_node(a.address)) return true;
        return false;
    }
    void add_address(const wire::Ipv6 &address, u8 prefix, u64 valid, u64 preferred)
    {
        if (auto *existing = find_address(address)) { existing->valid_until = valid; existing->preferred_until = preferred; return; }
        for (auto &a : addresses_)
            if (!a.used)
            { a = {true, address, prefix, Ipv6AddressState::tentative, valid, preferred, now_, 0}; return; }
    }
    void remove_address(Address &a) { a = {}; }

    void receive_arp(const wire::Ethernet &eth)
    {
        auto parsed = wire::arp(eth.payload);
        if (!parsed || !config_.ipv4) { add(stats_.dropped); return; }
        const auto &arp = parsed.value();
        const auto &self = config_.ipv4->address;
        if (arp.sender_mac != eth.source || wire::multicast(arp.sender_mac)) { add(stats_.dropped); return; }
        if (arp.sender_ip == self) { add(stats_.arp_conflicts); return; }
        const bool target = arp.target_ip == self;
        if (arp.operation == 1) add(stats_.arp_requests); else add(stats_.arp_replies);
        // RFC 826: merge into a known entry; create one only when addressed to us.
        if (unicast(arp.sender_ip) && (target || arp_.find(arp.sender_ip)))
        {
            const bool reply = arp.operation == 2;
            static_cast<void>(arp_.update(arp.sender_ip, arp.sender_mac,
                reply ? NeighbourUpdate::advertisement : NeighbourUpdate::announcement,
                {reply && target && eth.destination == config_.mac, true, false}, now_));
        }
        if (arp.operation != 1 || !target) return;
        auto built = wire::build_arp(std::span{scratch_}.subspan(14), 2, config_.mac, self, arp.sender_mac, arp.sender_ip);
        if (built) push(finish(scratch_, built.value(), arp.sender_mac, wire::ethertype_arp));
    }

    Received receive_ipv4(wire::Bytes payload, bool group)
    {
        if (!config_.ipv4) return filtered();
        auto parsed = wire::ipv4(payload); if (!parsed) return dropped();
        const auto &ip = parsed.value();
        const bool broadcast = wire::broadcast(ip.destination) || subnet_broadcast(ip.destination);
        if (!local(ip.destination) && !broadcast) return filtered();
        if (!unicast(ip.source) || local(ip.source) || subnet_broadcast(ip.source)) return dropped();
        const wire::Bytes packet = payload.first(wire::be16(payload, 2));
        if (ip.protocol == wire::protocol_icmp)
        {
            auto message = wire::icmp<wire::Ipv4Family>(ip); if (!message) return dropped();
            const auto &m = message.value();
            if (m.type == wire::icmpv4::echo_request)
            {
                // Echo to a broadcast address is ignored, as Linux does by default.
                if (broadcast || group) return filtered();
                add(stats_.echo_requests);
                echo<wire::Ipv4Family>(ip.destination, ip.source, wire::icmpv4::echo_reply, m);
                return consumed();
            }
            if (m.type == wire::icmpv4::echo_reply) { record_echo(4, ip.source, m); return consumed(); }
        }
        add(stats_.delivered);
        return {Delivery::ipv4, ip.protocol, packet, group || broadcast};
    }

    Received receive_ipv6(wire::Bytes payload, bool group)
    {
        if (!config_.ipv6) return filtered();
        auto parsed = wire::ipv6(payload); if (!parsed) return dropped();
        const auto &ip = parsed.value();
        const auto *own = find_address(ip.destination);
        const bool multicast = wire::multicast(ip.destination);
        if (multicast ? !joined(ip.destination) : !own) return filtered();
        // Our own source is a loop, except that a node defending an address
        // we hold as tentative sends from it.
        const auto *from = find_address(ip.source);
        if (from && from->state != Ipv6AddressState::tentative) return dropped();
        const wire::Bytes packet = payload.first(40 + wire::be16(payload, 4));
        if (ip.protocol == wire::protocol_icmpv6)
        {
            auto message = wire::icmp<wire::Ipv6Family>(ip); if (!message) return dropped();
            const auto &m = message.value();
            if (m.type >= wire::icmpv6::router_solicitation && m.type <= wire::icmpv6::neighbor_advertisement)
            {
                auto nd = wire::neighbor_discovery(ip, m); if (!nd) return dropped();
                receive_nd(ip, nd.value());
                return consumed();
            }
            if (own && own->state == Ipv6AddressState::tentative) return filtered();
            if (m.type == wire::icmpv6::echo_request)
            {
                if (multicast || group) return filtered();
                add(stats_.echo_requests);
                echo<wire::Ipv6Family>(ip.destination, ip.source, wire::icmpv6::echo_reply, m);
                return consumed();
            }
            if (m.type == wire::icmpv6::echo_reply) { record_echo(6, ip.source, m); return consumed(); }
        }
        if (wire::unspecified(ip.source) || (own && own->state == Ipv6AddressState::tentative)) return dropped();
        add(stats_.delivered);
        return {Delivery::ipv6, ip.protocol, packet, multicast || group};
    }

    void receive_nd(const wire::Ipv6Packet &ip, const wire::NdMessage &nd)
    {
        using wire::NdKind;
        if (nd.kind == NdKind::neighbor_solicitation)
        {
            add(stats_.solicitations);
            auto *own = find_address(nd.target);
            if (!own || own->state == Ipv6AddressState::duplicated) return;
            if (own->state == Ipv6AddressState::tentative)
            {
                // Another node probes the same tentative address (RFC 4862 5.4.3).
                if (wire::unspecified(ip.source)) duplicate(*own);
                return;
            }
            if (nd.source_link) static_cast<void>(nd_.update(ip.source, nd.source_link, NeighbourUpdate::announcement, {}, now_));
            const bool probe = wire::unspecified(ip.source);
            const auto destination = probe ? wire::all_nodes : ip.source;
            auto built = wire::build_neighbor_advertisement(std::span{scratch_}.subspan(14), nd.target, destination, nd.target,
                                                            false, !probe, true, config_.mac);
            if (built) static_cast<void>(send(built.value()));
            return;
        }
        if (nd.kind == NdKind::neighbor_advertisement)
        {
            add(stats_.advertisements);
            if (auto *own = find_address(nd.target))
            {
                if (own->state == Ipv6AddressState::tentative) duplicate(*own);
                else add(stats_.dad_conflicts);
                return;
            }
            const auto before = nd_.find(nd.target);
            static_cast<void>(nd_.update(nd.target, nd.target_link, NeighbourUpdate::advertisement,
                                         {nd.solicited, nd.override_flag, nd.router}, now_));
            // A router that stops advertising itself loses its default routes.
            if (before && before->router && !nd.router)
                routes6_.remove_if([&](const Route<wire::Ipv6> &r) { return r.gateway == nd.target; });
            return;
        }
        if (nd.kind == NdKind::router_advertisement)
        {
            add(stats_.router_advertisements); router_seen_ = true;
            if (nd.source_link) static_cast<void>(nd_.update(ip.source, nd.source_link, NeighbourUpdate::announcement, {}, now_));
            if (nd.router_lifetime)
                static_cast<void>(routes6_.add({{}, 0, ip.source, 0, 1024, RouteOrigin::router_advertisement,
                                               deadline(now_, u64{nd.router_lifetime} * 1000)}));
            else static_cast<void>(routes6_.remove({}, 0, ip.source, 0));
            if (nd.reachable_time) nd_.set_reachable_time(nd.reachable_time);
            if (nd.retransmit_timer) nd_.set_retransmit_time(nd.retransmit_timer);
            if (nd.mtu && *nd.mtu >= 1280 && *nd.mtu <= config_.mtu) mtu6_ = static_cast<u16>(*nd.mtu);
            for (usize i = 0; i < nd.prefix_count; ++i)
            {
                const auto &p = nd.prefixes[i];
                const u64 valid = p.valid_lifetime == ~u32{0} ? ~u64{0} : deadline(now_, u64{p.valid_lifetime} * 1000);
                const u64 preferred = p.preferred_lifetime == ~u32{0} ? ~u64{0} : deadline(now_, u64{p.preferred_lifetime} * 1000);
                if (p.on_link)
                {
                    if (p.valid_lifetime) static_cast<void>(routes6_.add({p.prefix, p.length, {}, 0, 256, RouteOrigin::router_advertisement, valid}));
                    else static_cast<void>(routes6_.remove(p.prefix, p.length, {}, 0));
                }
                if (p.autonomous && p.length == 64 && p.valid_lifetime)
                    add_address(wire::with_interface_id(mask_prefix(p.prefix, 64), config_.mac), 64, valid, preferred);
            }
        }
        // Router solicitations are for routers; a host ignores them.
    }

    void duplicate(Address &a)
    {
        add(stats_.dad_conflicts); a.state = Ipv6AddressState::duplicated;
    }

    template <wire::AddressFamily Family>
    void echo(const typename Family::Address &self, const typename Family::Address &peer, u8 type, const wire::Icmp &request)
    {
        auto built = wire::build_icmp<Family>(std::span{scratch_}.subspan(14), self, peer, default_hop_limit, next_identification(),
                                              type, 0, request.rest, request.body);
        if (built) { add(stats_.echo_replies); static_cast<void>(send(built.value())); }
    }
    template <class Source> void record_echo(u8 version, const Source &source, const wire::Icmp &m)
    {
        EchoReply reply{version, {}, m.identifier(), m.sequence(), m.body.size(), now_};
        for (usize i = 0; i < source.size(); ++i) reply.source[i] = source[i];
        last_echo_ = reply;
    }
    template <wire::AddressFamily Family>
    void send_error(const typename Family::Address &self, const typename Family::Address &peer, u8 type, u8 code, wire::Bytes quote)
    {
        // Token bucket: icmp_burst errors, one more every icmp_token_ms.
        const u64 refill = (now_ - icmp_refilled_) / icmp_token_ms;
        if (refill) { icmp_tokens_ = static_cast<u8>(refill + icmp_tokens_ > icmp_burst ? icmp_burst : refill + icmp_tokens_); icmp_refilled_ = now_; }
        if (!icmp_tokens_) { add(stats_.icmp_rate_limited); return; }
        --icmp_tokens_;
        auto built = wire::build_icmp_error<Family>(std::span{scratch_}.subspan(14), self, peer, next_identification(), type, code, 0, quote);
        if (built) { add(stats_.icmp_errors); static_cast<void>(send(built.value())); }
    }
    void solicit(const Solicitation<wire::Ipv4> &s)
    {
        if (!config_.ipv4) return;
        auto built = wire::build_arp(std::span{scratch_}.subspan(14), 1, config_.mac, config_.ipv4->address, {}, s.target);
        if (built) push(finish(scratch_, built.value(), s.unicast ? *s.unicast : wire::broadcast_mac, wire::ethertype_arp));
    }
    void solicit(const Solicitation<wire::Ipv6> &s)
    {
        auto from = source(s.target); if (!from) return;
        const auto destination = s.unicast ? s.target : wire::solicited_node(s.target);
        auto built = wire::build_neighbor_solicitation(std::span{scratch_}.subspan(14), from.value(), destination, s.target, config_.mac);
        if (!built) return;
        push(finish(scratch_, built.value(), s.unicast ? *s.unicast : wire::multicast_mac(destination), wire::ethertype_ipv6));
    }
    // Routes a packet built in scratch_ and queues the resulting frame.
    Status send(usize length)
    {
        auto framed = output(scratch_, length); if (!framed) return framed.status();
        if (framed.value()) push(framed);
        return Status::success();
    }
    void push(Result<usize> length)
    {
        if (!length) return;
        if (control_count_ == control_capacity) { add(stats_.control_dropped); return; }
        auto &f = control_[(control_head_ + control_count_) % control_capacity];
        f.length = length.value();
        for (usize i = 0; i < f.length; ++i) f.bytes[i] = scratch_[i];
        ++control_count_;
    }
    Result<usize> finish(std::span<std::byte> frame, usize length, const wire::Mac &destination, u16 type)
    {
        auto header = wire::build_ethernet(frame, destination, config_.mac, type, length);
        if (!header) return header.status();
        usize total = header.value();
        for (; total < minimum_frame; ++total) frame[total] = std::byte{};
        return total;
    }
    template <class Address, class Routes, class Cache>
    Result<usize> routed(std::span<std::byte> frame, usize length, const Address &destination, Routes &routes, Cache &cache, u16 type)
    {
        auto route = routes.lookup(destination, now_);
        if (!route) { add(stats_.no_route); return route.status(); }
        auto mac = cache.resolve(route.value().next_hop, now_);
        if (mac) return finish(frame, length, mac.value(), type);
        if (mac.status().code() != StatusCode::would_block) { add(stats_.dropped); return usize{0}; }
        add(stats_.held);
        static_cast<void>(cache.hold(route.value().next_hop, wire::Bytes{frame.data() + 14, length}));
        return usize{0};
    }

    InterfaceConfig config_{};
    bool configured_{}, router_seen_{};
    u16 mtu6_{}, identification_{};
    u64 now_{}, solicit_deadline_{~u64{0}}, icmp_refilled_{};
    u8 solicitations_{}, icmp_tokens_{icmp_burst};
    Arp arp_{};
    Nd nd_{};
    RouteTable<wire::Ipv4> routes4_{};
    RouteTable<wire::Ipv6> routes6_{};
    std::array<Address, max_ipv6_addresses> addresses_{};
    std::array<Frame, control_capacity> control_{};
    usize control_head_{}, control_count_{};
    std::optional<EchoReply> last_echo_{};
    InterfaceStats stats_{};
    std::array<std::byte, frame_capacity> scratch_{}, flush_{};
};
}
