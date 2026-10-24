#pragma once

#include <ok/net/wire.hpp>

namespace ok::net::wire
{
inline constexpr u8 tcp_fin = 0x01, tcp_syn = 0x02, tcp_rst = 0x04, tcp_psh = 0x08;
inline constexpr u8 tcp_ack = 0x10, tcp_urg = 0x20, tcp_ece = 0x40, tcp_cwr = 0x80;

struct TcpOption
{
    u8 kind{};
    Bytes bytes{}, data{};
};

// Options borrow the retained, immutable packet. Unknown TLVs remain opaque;
// this cursor validates wire shape without negotiating their transport meaning.
class TcpOptions final
{
  public:
    explicit TcpOptions(Bytes options, u8 flags) : options_(options), flags_(flags) {}

    Result<std::optional<TcpOption>> next()
    {
        if (options_.size() > 40) return Status::invalid_argument("TCP options exceed the base header limit");
        if (at_ == options_.size()) return std::optional<TcpOption>{};
        const u8 kind = byte(options_, at_);
        if (kind == 0)
        {
            for (usize i = at_ + 1; i < options_.size(); ++i)
                if (byte(options_, i)) return Status::invalid_argument("TCP EOL padding is not zero");
            const auto bytes = options_.subspan(at_, 1);
            at_ = options_.size();
            return std::optional<TcpOption>{TcpOption{kind, bytes, {}}};
        }
        if (kind == 1)
        {
            const auto bytes = options_.subspan(at_, 1);
            ++at_;
            return std::optional<TcpOption>{TcpOption{kind, bytes, {}}};
        }
        const usize remaining = options_.size() - at_;
        if (remaining < 2) return Status::invalid_argument("truncated TCP option length");
        const usize length = byte(options_, at_ + 1);
        if (length < 2 || length > remaining) return Status::invalid_argument("invalid TCP option length");
        if (kind == 2 && (length != 4 || !(flags_ & tcp_syn)))
            return Status::invalid_argument("TCP MSS requires four bytes and SYN");
        const auto bytes = options_.subspan(at_, length);
        at_ += length;
        return std::optional<TcpOption>{TcpOption{kind, bytes, bytes.subspan(2)}};
    }

  private:
    Bytes options_{};
    usize at_{};
    u8 flags_{};
};

namespace tcp_detail
{
inline u32 be32(Bytes bytes, usize at)
{
    return (u32{be16(bytes, at)} << 16) | be16(bytes, at + 2);
}
inline void be32(std::span<std::byte> bytes, usize at, u32 value)
{
    be16(bytes, at, static_cast<u16>(value >> 16));
    be16(bytes, at + 2, static_cast<u16>(value));
}
inline Status options(Bytes bytes, u8 flags)
{
    TcpOptions cursor{bytes, flags};
    for (;;)
    {
        auto option = cursor.next();
        if (!option) return option.status();
        if (!option.value()) return Status::success();
    }
}
inline bool overlaps(Bytes input, std::span<std::byte> output)
{
    if (input.empty() || output.empty()) return false;
    const uptr first = reinterpret_cast<uptr>(output.data());
    const uptr source = reinterpret_cast<uptr>(input.data());
    return source >= first ? source - first < output.size() : first - source < input.size();
}
}

struct TcpFields
{
    u16 source_port{}, destination_port{};
    u32 sequence{}, acknowledgment{};
    u8 flags{};
    u16 window{}, urgent_pointer{};
};
struct Tcp
{
    TcpFields fields{};
    Bytes options{}, payload{};
};

// The caller retains the IPv4 packet; all returned spans borrow its bytes.
// RFC 9293 section 3.1 requires receiving hosts to ignore reserved bits.
inline Result<Tcp> tcp(const Ipv4Packet &ip)
{
    if (ip.protocol != 6) return Status::unsupported("IPv4 payload is not TCP");
    const auto segment = ip.payload;
    if (segment.size() < 20 || segment.size() > 65515)
        return Status::invalid_argument("invalid IPv4 TCP segment length");
    const usize header = (byte(segment, 12) >> 4) * 4u;
    if (header < 20 || header > segment.size()) return Status::invalid_argument("invalid TCP header length");
    if (sum(segment, pseudo_sum(ip.source, ip.destination, 6, static_cast<u16>(segment.size()))) != 0xffffu)
        return Status::invalid_argument("invalid IPv4 TCP pseudo-header checksum");
    const u8 flags = byte(segment, 13);
    const auto options = segment.subspan(20, header - 20);
    const auto status = tcp_detail::options(options, flags);
    if (!status) return status;
    return Tcp{TcpFields{be16(segment, 0), be16(segment, 2), tcp_detail::be32(segment, 4),
        tcp_detail::be32(segment, 8), flags, be16(segment, 14), be16(segment, 18)},
        options, segment.subspan(header)};
}

// Options and payload must not overlap the emitted prefix. Input in an unused
// output tail is allowed. Every invalid call leaves the entire output unchanged.
inline Result<usize> build_tcp(std::span<std::byte> output, Ipv4 source, Ipv4 destination,
    u16 identification, u8 ttl, TcpFields fields, Bytes options, Bytes payload)
{
    if (!ttl) return Status::invalid_argument("TCP send TTL must be nonzero");
    const auto valid_options = tcp_detail::options(options, fields.flags);
    if (!valid_options) return valid_options;
    const usize header = 20 + ((options.size() + 3) & ~usize{3});
    if (payload.size() > 65515 - header) return Status::overflow("IPv4 TCP payload exceeds packet limit");
    const usize length = 20 + header + payload.size();
    if (output.size() < length) return Status::overflow("IPv4 TCP packet exceeds output capacity");
    const auto emitted = output.first(length);
    if (tcp_detail::overlaps(options, emitted) || tcp_detail::overlaps(payload, emitted))
        return Status::invalid_argument("IPv4 TCP input aliases output");
    for (usize i = 0; i < 20 + header; ++i) output[i] = std::byte{};
    output[0] = std::byte{0x45}; output[8] = std::byte{ttl}; output[9] = std::byte{6};
    be16(output, 2, static_cast<u16>(length)); be16(output, 4, identification); be16(output, 6, 0x4000);
    for (usize i = 0; i < 4; ++i)
    {
        output[12 + i] = std::byte{source[i]}; output[16 + i] = std::byte{destination[i]};
    }
    be16(output, 10, checksum(Bytes{output.data(), 20}));
    be16(output, 20, fields.source_port); be16(output, 22, fields.destination_port);
    tcp_detail::be32(output, 24, fields.sequence); tcp_detail::be32(output, 28, fields.acknowledgment);
    output[32] = std::byte{static_cast<u8>((header / 4) << 4)}; output[33] = std::byte{fields.flags};
    be16(output, 34, fields.window); be16(output, 38, fields.urgent_pointer);
    for (usize i = 0; i < options.size(); ++i) output[40 + i] = options[i];
    for (usize i = 0; i < payload.size(); ++i) output[20 + header + i] = payload[i];
    const u16 transport_checksum = static_cast<u16>(~sum(Bytes{output.data() + 20, length - 20},
        pseudo_sum(source, destination, 6, static_cast<u16>(length - 20))));
    be16(output, 36, transport_checksum);
    return length;
}
}
