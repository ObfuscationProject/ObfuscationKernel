#pragma once

#include <ok/net/interface.hpp>
#include <ok/net/native_tcp_stream.hpp>
#include <optional>
#include <string_view>

namespace ok::net
{
// A link self-test the distribution may ask for (kernel diagnostics). It
// pings the IPv4 gateway and the IPv6 router, reports the autoconfigured
// addresses, and exchanges `tcp_bytes` with a TCP echo service.
struct ProbePlan
{
    bool ping{true};
    std::optional<tcp::Endpoint> tcp_echo{};
    u16 tcp_bytes{4096};
    u64 deadline_ms{60000};
};

struct ProbeResult
{
    bool link{}, arp{}, ping4{}, ipv6{}, ping6{}, tcp{}, finished{}, passed{};
};

// Serialized with the network executor; called after each executor pass.
// Markers (`OK_NET_*`) go to `emit`, one complete line per event.
class LinkProbe final
{
  public:
    static constexpr u16 identifier = 0x4f4b;
    static constexpr u64 retry_ms = 1000;
    static constexpr u16 local_port = 49200;
    LinkProbe() = default;
    LinkProbe(const LinkProbe &) = delete;
    LinkProbe &operator=(const LinkProbe &) = delete;

    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] const ProbeResult &result() const { return result_; }
    void arm(const ProbePlan &plan) { plan_ = plan; armed_ = true; }

    template <class Emit, tcp::IssSource Iss>
        requires requires(Emit &emit, std::string_view text) { emit(text); }
    void step(EthernetInterface &nic, NativeTcpStreamService &tcp, u64 now, Iss &iss, Emit &&emit)
    {
        if (!armed_ || result_.finished || !nic.configured()) return;
        Line line{emit};
        if (!started_) { started_ = true; start_ = now; report_link(nic, line); }
        const auto &config = nic.config();
        if (config.ipv4 && config.ipv4->gateway)
        {
            const auto gateway = *config.ipv4->gateway;
            if (!result_.arp)
                if (auto entry = nic.arp().find(gateway); entry && entry->state != NeighbourState::incomplete &&
                    entry->state != NeighbourState::permanent)
                {
                    result_.arp = true;
                    line.text("OK_NET_ARP scope=hardware gateway="); line.ipv4(gateway);
                    line.text(" mac="); line.mac(entry->mac); line.end();
                }
            if (plan_.ping && !result_.ping4) ping<wire::Ipv4Family>(nic, gateway, 1, now, sent4_, line);
        }
        if (config.ipv6 && !result_.ipv6) report_ipv6(nic, line);
        if (plan_.ping && result_.ipv6 && !result_.ping6 && router_) ping<wire::Ipv6Family>(nic, *router_, 2, now, sent6_, line);
        if (plan_.tcp_echo && !result_.tcp) echo(nic, tcp, now, iss, line);
        const bool done = (!plan_.ping || (result_.ping4 && (!config.ipv6 || result_.ping6))) &&
            (!config.ipv6 || result_.ipv6) && (!plan_.tcp_echo || result_.tcp);
        if (done || now - start_ >= plan_.deadline_ms || failed_)
        {
            result_.finished = true; result_.passed = done && !failed_;
            line.text("OK_NET_PROBE scope=hardware result="); line.text(result_.passed ? "pass" : "fail");
            line.text(" link="); line.flag(result_.link); line.text(" arp="); line.flag(result_.arp);
            line.text(" ping4="); line.flag(result_.ping4); line.text(" ipv6="); line.flag(result_.ipv6);
            line.text(" ping6="); line.flag(result_.ping6); line.text(" tcp="); line.flag(result_.tcp);
            if (failed_) { line.text(" failure="); line.text(failure_); }
            line.end();
        }
    }

  private:
    template <class Emit> struct Line
    {
        Emit &emit;
        void text(std::string_view value) { emit(value); }
        void flag(bool value) { emit(value ? std::string_view{"1"} : std::string_view{"0"}); }
        void number(u64 value)
        {
            char digits[20]; usize count = 0;
            do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
            char out[20]; for (usize i = 0; i < count; ++i) out[i] = digits[count - 1 - i];
            emit(std::string_view{out, count});
        }
        void hex(u64 value, usize width)
        {
            char out[16]; constexpr std::string_view alphabet{"0123456789abcdef"};
            for (usize i = 0; i < width; ++i) out[width - 1 - i] = alphabet[(value >> (4 * i)) & 15u];
            emit(std::string_view{out, width});
        }
        void ipv4(const wire::Ipv4 &a) { for (usize i = 0; i < 4; ++i) { if (i) text("."); number(a[i]); } }
        void mac(const wire::Mac &m) { for (usize i = 0; i < 6; ++i) { if (i) text(":"); hex(m[i], 2); } }
        // RFC 5952 text form: lowercase, leading zeros dropped, longest zero run as "::".
        void ipv6(const wire::Ipv6 &a)
        {
            u16 groups[8]; for (usize i = 0; i < 8; ++i) groups[i] = static_cast<u16>((u16{a[2 * i]} << 8) | a[2 * i + 1]);
            usize best = 8, best_length = 0;
            for (usize i = 0; i < 8;)
            {
                if (groups[i]) { ++i; continue; }
                usize j = i; while (j < 8 && !groups[j]) ++j;
                if (j - i > best_length && j - i > 1) { best = i; best_length = j - i; }
                i = j;
            }
            for (usize i = 0; i < 8;)
            {
                if (i == best) { text("::"); i += best_length; continue; }
                if (i && i != best + best_length) text(":");
                u16 g = groups[i]; usize width = g >= 0x1000 ? 4 : g >= 0x100 ? 3 : g >= 0x10 ? 2 : 1;
                hex(g, width); ++i;
            }
        }
        void end() { emit(std::string_view{"\n"}); }
    };
    template <class Emit> void report_link(EthernetInterface &nic, Line<Emit> &line)
    {
        const auto &config = nic.config();
        result_.link = true;
        line.text("OK_NET_LINK scope=hardware mac="); line.mac(config.mac);
        if (config.ipv4)
        {
            line.text(" ipv4="); line.ipv4(config.ipv4->address); line.text("/"); line.number(config.ipv4->prefix);
            if (config.ipv4->gateway) { line.text(" gateway="); line.ipv4(*config.ipv4->gateway); }
        }
        line.text(" ipv6="); line.text(config.ipv6 ? "on" : "off"); line.end();
    }
    template <class Emit> void report_ipv6(EthernetInterface &nic, Line<Emit> &line)
    {
        std::optional<wire::Ipv6> local, global;
        nic.each_ipv6([&](const Ipv6AddressSnapshot &a) {
            if (a.state != Ipv6AddressState::preferred) return;
            if (wire::link_local(a.address)) local = a.address; else global = a.address;
        });
        std::optional<wire::Ipv6> router;
        nic.routes6().each([&](const Route<wire::Ipv6> &r) { if (!r.length && r.gateway) router = r.gateway; });
        if (!local || !global || !router) return;
        result_.ipv6 = true; router_ = router;
        line.text("OK_NET_IPV6 scope=hardware link_local="); line.ipv6(*local);
        line.text(" global="); line.ipv6(*global); line.text(" router="); line.ipv6(*router);
        line.text(" mtu="); line.number(nic.ipv6_mtu()); line.end();
    }
    template <wire::AddressFamily Family, class Emit>
    void ping(EthernetInterface &nic, const typename Family::Address &peer, u16 sequence, u64 now, u64 &sent, Line<Emit> &line)
    {
        const auto reply = nic.last_echo_reply();
        if (reply && reply->identifier == identifier && reply->sequence == sequence && reply->version == Family::version && sent)
        {
            (Family::version == 4 ? result_.ping4 : result_.ping6) = true;
            line.text("OK_NET_PING scope=hardware family="); line.number(Family::version);
            line.text(" peer=");
            if constexpr (Family::version == 4) line.ipv4(peer); else line.ipv6(peer);
            line.text(" sequence="); line.number(sequence);
            line.text(" rtt_ms="); line.number(reply->at >= sent_at(sequence) ? reply->at - sent_at(sequence) : 0);
            line.end();
            return;
        }
        if (sent && now - sent < retry_ms) return;
        static constexpr std::array<std::byte, 16> payload{std::byte{'O'}, std::byte{'K'}, std::byte{'N'}, std::byte{'E'},
            std::byte{'T'}, std::byte{'P'}, std::byte{'I'}, std::byte{'N'}, std::byte{'G'}};
        if (nic.ping<Family>(peer, identifier, sequence, payload)) { sent = now ? now : 1; if (!sent_at(sequence)) sent_at(sequence) = now; }
    }
    u64 &sent_at(u16 sequence) { return sequence == 1 ? first4_ : first6_; }
    template <class Emit, tcp::IssSource Iss>
    void echo(EthernetInterface &nic, NativeTcpStreamService &service, u64 now, Iss &iss, Line<Emit> &line)
    {
        auto &table = service.endpoints();
        const auto &peer = *plan_.tcp_echo;
        if (!connection_)
        {
            if (connect_attempted_) return;
            connect_attempted_ = true;
            const auto &config = nic.config();
            if (!config.ipv4 || !peer.address.v4()) { fail("tcp-address"); return; }
            const tcp::Endpoint local{config.ipv4->address, local_port};
            auto value = iss(local, peer); if (!value) { fail("tcp-iss"); return; }
            auto handle = table.connect(local, peer, value.value(), now); if (!handle) { fail("tcp-connect"); return; }
            connection_ = handle.value(); tcp_started_ = now;
            return;
        }
        auto state = table.state(*connection_); if (!state) { fail("tcp-state"); return; }
        const auto &transport = state.value().transport;
        if (transport.failure != tcp::Failure::none) { fail("tcp-failure"); return; }
        if (transport.state == tcp::State::syn_sent || transport.state == tcp::State::syn_received) return;
        // Pattern bytes go out as the send ring takes them; the echo must match.
        while (written_ < plan_.tcp_bytes)
        {
            std::array<std::byte, 512> chunk{};
            const usize left = usize{plan_.tcp_bytes} - written_, count = left < chunk.size() ? left : chunk.size();
            for (usize i = 0; i < count; ++i) chunk[i] = pattern(written_ + i);
            auto taken = table.write(*connection_, wire::Bytes{chunk.data(), count});
            if (!taken) { if (taken.status().code() == StatusCode::would_block) break; fail("tcp-write"); return; }
            if (!taken.value()) break;
            written_ += static_cast<u16>(taken.value());
        }
        for (;;)
        {
            std::array<std::byte, 512> chunk{};
            auto got = table.read(*connection_, chunk);
            if (!got) { if (got.status().code() == StatusCode::would_block) break; fail("tcp-read"); return; }
            if (!got.value()) break;
            for (usize i = 0; i < got.value(); ++i)
                if (chunk[i] != pattern(echoed_ + i)) { fail("tcp-mismatch"); return; }
            echoed_ += static_cast<u16>(got.value());
        }
        if (echoed_ < plan_.tcp_bytes) return;
        result_.tcp = true;
        const auto snapshot = table.state(*connection_).value().transport;
        line.text("OK_NET_TCP scope=hardware peer="); line.ipv4(peer.address.ipv4()); line.text(":"); line.number(peer.port);
        line.text(" bytes="); line.number(plan_.tcp_bytes); line.text(" echoed="); line.number(echoed_);
        line.text(" cwnd="); line.number(snapshot.congestion_window); line.text(" rto_ms="); line.number(snapshot.rto_ms);
        line.text(" elapsed_ms="); line.number(now - tcp_started_); line.end();
        static_cast<void>(table.shutdown_write(*connection_));
        static_cast<void>(table.release(*connection_, now));
        connection_.reset();
    }
    static std::byte pattern(usize at) { return std::byte{static_cast<u8>((at * 7u + (at >> 8)) & 0xffu)}; }
    void fail(std::string_view reason) { failed_ = true; failure_ = reason; }

    ProbePlan plan_{};
    ProbeResult result_{};
    bool armed_{}, started_{}, failed_{}, connect_attempted_{};
    std::string_view failure_{};
    u64 start_{}, sent4_{}, sent6_{}, first4_{}, first6_{}, tcp_started_{};
    std::optional<wire::Ipv6> router_{};
    std::optional<tcp::Handle> connection_{};
    u16 written_{}, echoed_{};
};
}
