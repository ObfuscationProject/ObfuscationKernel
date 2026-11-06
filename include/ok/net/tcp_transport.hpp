#pragma once

#include <ok/net/tcp_wire.hpp>
#include <ok/net/tcp_congestion.hpp>
#include <ok/net/address.hpp>
#include <limits>

namespace ok::net::tcp
{
using wire::Bytes;
using wire::Ipv4;
using wire::TcpFields;
using namespace wire;

// Serial comparisons are valid for distances below half the sequence space.
inline constexpr bool before(u32 a, u32 b) { return a != b && u32{b - a} < 0x80000000u; }
inline constexpr bool after(u32 a, u32 b) { return before(b, a); }
inline constexpr bool between(u32 value, u32 first, u32 last)
{ return u32{last - first} < 0x80000000u && u32{value - first} <= u32{last - first}; }

struct Endpoint { IpAddress address{}; u16 port{}; bool operator==(const Endpoint &) const = default; };
enum class State : u8 { closed, listen, syn_sent, syn_received, established, fin_wait_1,
    fin_wait_2, close_wait, closing, last_ack, time_wait };
enum class Failure : u8 { none, reset, refused, timed_out, aborted };
enum class Ingress : u8 { ignored, accepted, challenged, reset };
struct PreparedPacket { usize length{}; u64 token{}; };
struct Snapshot
{
    State state{}; Failure failure{};
    u32 send_unacknowledged{}, send_next{}, receive_next{};
    usize send_buffered{}, receive_buffered{};
    u16 peer_window{}, receive_window{}, peer_mss{};
    u64 retransmit_deadline{}, time_wait_deadline{}, rto_ms{};
    u8 retransmissions{}; bool flight{}, eof{}, write_shutdown{}, prepared{}, listener_armed{};
    u32 congestion_window{}, slow_start_threshold{}, in_flight{};
    usize out_of_order{}; u8 duplicate_acks{}; bool recovering{};
};

// One executor owns this noncopyable control block. Input and write bytes are
// copied before return; no wire/user span is retained. Each prepare grants one
// packet token, which must be completed before another state-changing operation.
// Caller owns emitted bytes; accepted device TX never releases the retained
// stream bytes. Only a validated peer ACK releases retransmittable storage.
// Several segments may be in flight, bounded by the peer window and the
// congestion window of the static `Congestion` policy. One retransmission
// timer covers [SND.UNA, SND.MAX); a timeout resends from SND.UNA. Segments
// beyond RCV.NXT are kept in the receive ring and joined when the gap fills.
template <usize SendCapacity = 4096, usize ReceiveCapacity = 4096, usize SegmentCapacity = 536,
          CongestionControl Congestion = NewReno>
class Connection final
{
    static_assert(SendCapacity > 0 && SendCapacity <= 65535);
    static_assert(ReceiveCapacity > 0 && ReceiveCapacity <= 65535);
    static_assert(SegmentCapacity > 0 && SegmentCapacity <= 1460);
  public:
    static constexpr u64 msl_ms = 120000, max_rto_ms = 60000;
    static constexpr u8 max_retransmissions = 8, fast_retransmit_threshold = 3;
    static constexpr usize max_out_of_order = 4;
    // Constant-initializable: every open path starts from reset_fields().
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;

    [[nodiscard]] Snapshot snapshot() const
    {
        return {state_, failure_, snd_una_, snd_nxt_, rcv_nxt_, tx_size_, rx_size_, peer_window_,
            window(), peer_mss_, rtx_.active ? rtx_.deadline : 0, time_wait_deadline_, rto_,
            rtx_.retries, rtx_.active, eof_, write_shutdown_, plan_.active, listener_armed_,
            cc_.window(), cc_.threshold(), outstanding(), out_of_order(), dupacks_, recovery_};
    }
    [[nodiscard]] Endpoint local() const { return local_; }
    [[nodiscard]] Endpoint peer() const { return peer_; }

    // ISS is supplied by the caller. This core provides no entropy and must not
    // be exposed to an untrusted network with a predictable sequence source.
    Status active_open(Endpoint local, Endpoint peer, u32 iss, u64 now)
    {
        if (plan_.active || state_ != State::closed || rx_size_) return Status::busy("TCP control block is in use");
        if (!valid_endpoint(local) || !valid_endpoint(peer) || local.address.family != peer.address.family)
            return Status::invalid_argument("invalid TCP endpoints");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        reset_fields(); local_ = local; peer_ = peer; start_sequence(iss);
        state_ = State::syn_sent; pending_syn_ = true; last_now_ = now;
        return Status::success();
    }
    Status passive_open(Endpoint local, u32 iss, u64 now)
    {
        if (plan_.active || state_ != State::closed || rx_size_) return Status::busy("TCP control block is in use");
        if (!valid_endpoint(local)) return Status::invalid_argument("invalid TCP listener endpoint");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        reset_fields(); local_ = local; start_sequence(iss);
        state_ = State::listen; passive_ = true; listener_armed_ = true; last_now_ = now;
        return Status::success();
    }
    // A failed passive incarnation consumes its supplied ISS. The executor must
    // provide a fresh unpredictable value before the returned listener is used.
    Status rearm_listener(u32 iss, u64 now)
    {
        if (plan_.active || state_ != State::listen || listener_armed_ || reset_pending_)
            return Status::busy("TCP listener is not awaiting a fresh ISS");
        if (now < last_now_ || iss == iss_) return Status::invalid_argument("fresh listener ISS and monotonic time required");
        start_sequence(iss); listener_armed_ = true; last_now_ = now;
        return Status::success();
    }
    Status abort()
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before abort");
        terminate(Failure::aborted); return Status::success();
    }
    Result<usize> write(Bytes input)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before write");
        if (write_shutdown_ || (state_ != State::established && state_ != State::close_wait))
            return Status::fault("TCP stream is not writable");
        if (aliases_self(input)) return Status::invalid_argument("TCP write aliases control-block storage");
        if (input.empty()) return usize{0};
        const usize count = min(input.size(), SendCapacity - tx_size_);
        if (!count) return Status::would_block("TCP send buffer is full");
        for (usize i = 0; i < count; ++i) tx_[(tx_head_ + tx_size_ + i) % SendCapacity] = input[i];
        tx_size_ += count; return count;
    }
    // A serialized checked copy can preflight/copy output before consuming bytes.
    Result<usize> peek(std::span<std::byte> output) const
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before peek");
        if (aliases_self(Bytes{output.data(), output.size()})) return Status::invalid_argument("TCP peek aliases control-block storage");
        if (failure_ != Failure::none) return Status::fault("TCP stream failed");
        if (output.empty()) return usize{0};
        if (!rx_size_) return eof_ ? Result<usize>{usize{0}} : Result<usize>{Status::would_block("TCP receive buffer is empty")};
        const usize count = min(output.size(), rx_size_);
        for (usize i = 0; i < count; ++i) output[i] = rx_[(rx_head_ + i) % ReceiveCapacity];
        return count;
    }
    Result<usize> read(std::span<std::byte> output)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before read");
        if (aliases_self(Bytes{output.data(), output.size()})) return Status::invalid_argument("TCP read aliases control-block storage");
        if (failure_ != Failure::none) return Status::fault("TCP stream failed");
        if (output.empty()) return usize{0};
        if (!rx_size_) return eof_ ? Result<usize>{usize{0}} : Result<usize>{Status::would_block("TCP receive buffer is empty")};
        const usize count = min(output.size(), rx_size_);
        for (usize i = 0; i < count; ++i) output[i] = rx_[(rx_head_ + i) % ReceiveCapacity];
        rx_head_ = (rx_head_ + count) % ReceiveCapacity; rx_size_ -= count;
        if (state_ != State::closed && state_ != State::listen) pending_ack_ = true;
        return count;
    }
    Status shutdown_write()
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before shutdown");
        if (state_ != State::established && state_ != State::close_wait) return Status::fault("TCP stream is not connected");
        write_shutdown_ = true; return Status::success();
    }

    // This accepts complete IPv4 or IPv6 bytes, not caller-forged parsed
    // fields. The codecs validate IP and mandatory TCP checksums before any state.
    Result<Ingress> receive(Bytes packet, u64 now)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before ingress");
        if (aliases_self(packet)) return Status::invalid_argument("TCP ingress aliases control-block storage");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        if (wire::ip_version(packet) == 6)
        {
            auto ip = wire::ipv6(packet); if (!ip) return ip.status();
            auto tcp = wire::tcp<wire::Ipv6Family>(ip.value()); if (!tcp) return tcp.status();
            return ingress(ip.value().source, ip.value().destination, tcp.value(), now);
        }
        auto ip = wire::ipv4(packet); if (!ip) return ip.status();
        auto tcp = wire::tcp(ip.value()); if (!tcp) return tcp.status();
        return ingress(ip.value().source, ip.value().destination, tcp.value(), now);
    }

  private:
    Result<Ingress> ingress(const IpAddress &from, const IpAddress &to, const wire::Tcp &seg, u64 now)
    {
        const auto &f = seg.fields;
        if (to != local_.address || f.destination_port != local_.port || !f.source_port)
            return Ingress::ignored;
        const Endpoint source{from, f.source_port};
        if (!valid_endpoint(source) || (state_ != State::listen && source != peer_)) return Ingress::ignored;
        last_now_ = now;
        if (state_ == State::closed) return Ingress::ignored;
        // RFC 1337: a reset cannot assassinate TIME_WAIT or discard unread data.
        if (state_ == State::time_wait && (f.flags & tcp_rst)) return Ingress::ignored;
        // URG semantics are not implemented. Do not silently deliver it as
        // ordinary stream data or claim urgent-pointer/socket compatibility.
        if (f.flags & tcp_urg) return Status::unsupported("TCP urgent data is not supported by this bounded core");
        if (state_ == State::listen)
        {
            if (f.flags & tcp_rst) return Ingress::ignored;
            if (f.flags & tcp_ack) { queue_reset(source, f.acknowledgment); return Ingress::accepted; }
            if (!(f.flags & tcp_syn) || (f.flags & tcp_fin)) return Ingress::ignored;
            // SYN payload is left unacknowledged and must be retransmitted after
            // the handshake; this core does not negotiate TCP Fast Open.
            if (!listener_armed_) return Status::not_initialized("passive incarnation requires a fresh ISS");
            listener_armed_ = false; peer_ = source; irs_ = f.sequence; rcv_nxt_ = f.sequence + 1;
            peer_window_ = f.window; wl1_ = f.sequence; wl2_ = 0; negotiate_mss(seg);
            state_ = State::syn_received; pending_syn_ = true; return Ingress::accepted;
        }
        if (state_ == State::syn_sent)
        {
            const bool ack = (f.flags & tcp_ack) != 0;
            if (ack && (f.acknowledgment != iss_ + 1 || snd_max_ != iss_ + 1))
            {
                if (!(f.flags & tcp_rst)) queue_reset(peer_, f.acknowledgment);
                return Ingress::ignored;
            }
            if (f.flags & tcp_rst)
            {
                if (ack) { terminate(Failure::refused); return Ingress::reset; }
                return Ingress::ignored;
            }
            if (!(f.flags & tcp_syn) || (f.flags & tcp_fin)) return Ingress::ignored;
            irs_ = f.sequence; rcv_nxt_ = f.sequence + 1; peer_window_ = f.window;
            wl1_ = f.sequence; wl2_ = ack ? f.acknowledgment : 0; negotiate_mss(seg);
            if (ack)
            {
                acknowledge(f.acknowledgment, now); state_ = State::established;
                if (syn_retried_ && rto_ < 3000) rto_ = 3000;
                pending_syn_ = false; pending_ack_ = true;
                // No SYN payload is ACKed beyond the SYN sequence number.
            }
            else { state_ = State::syn_received; pending_syn_ = true; }
            return Ingress::accepted;
        }
        // Retain TIME_WAIT for duplicate FINs, including FIN's consumed sequence
        // just below RCV.NXT, instead of rejecting it as an old segment.
        if (state_ == State::time_wait && (f.flags & tcp_fin) && !(f.flags & (tcp_rst | tcp_syn)) &&
            (f.flags & tcp_ack) && f.sequence + static_cast<u32>(seg.payload.size()) + 1 == rcv_nxt_)
        {
            if (!between(f.acknowledgment, snd_una_ - 65535u, snd_max_)) return challenge(now);
            pending_ack_ = true; time_wait_deadline_ = deadline(now, 2 * msl_ms); return Ingress::accepted;
        }
        if (state_ == State::syn_received && passive_ && f.sequence == irs_ &&
            (f.flags & tcp_syn) && !(f.flags & (tcp_ack | tcp_rst | tcp_fin)))
        {
            // A lost SYN-ACK must not elicit an ACK-only response: SYN-SENT
            // rejects it. Retain the original SYN sequence, and respect RTO.
            if (!rtx_.active || now >= rtx_.deadline) pending_syn_ = true;
            return Ingress::accepted;
        }
        if (state_ == State::syn_received && !passive_ && (f.flags & tcp_syn) &&
            f.sequence == irs_ && (f.flags & tcp_ack) && !(f.flags & (tcp_rst | tcp_fin)))
            return receive_syn(seg, now);
        const u32 length = static_cast<u32>(seg.payload.size()) + ((f.flags & tcp_syn) ? 1u : 0u) + ((f.flags & tcp_fin) ? 1u : 0u);
        if (!acceptable(f.sequence, length))
        {
            if (!(f.flags & tcp_rst)) pending_ack_ = true;
            return Ingress::ignored;
        }
        if (f.flags & tcp_rst)
        {
            if (f.sequence != rcv_nxt_) return challenge(now);
            if (state_ == State::syn_received && passive_) return_to_listen();
            else terminate(Failure::reset);
            return Ingress::reset;
        }
        // A simultaneous-open SYN|ACK may repeat the already consumed peer SYN.
        // It is handled below by the dedicated duplicate-SYN path in receive_syn.
        if (f.flags & tcp_syn) return receive_syn(seg, now);
        if (!(f.flags & tcp_ack)) return Ingress::ignored;
        if (!between(f.acknowledgment, snd_una_ - 65535u, snd_max_)) return challenge(now);
        const u32 previous_una = snd_una_; const u16 previous_window = peer_window_;
        if (state_ == State::syn_received)
        {
            if (f.acknowledgment != snd_max_ || snd_max_ != iss_ + 1)
            { queue_reset(peer_, f.acknowledgment); return Ingress::ignored; }
            acknowledge(f.acknowledgment, now); state_ = State::established; pending_syn_ = false;
            if (syn_retried_ && rto_ < 3000) rto_ = 3000;
        }
        else if (after(f.acknowledgment, snd_una_)) acknowledge(f.acknowledgment, now);
        if (state_ == State::closed) return Ingress::accepted;
        if (between(f.acknowledgment, snd_una_, snd_max_) &&
            (after(f.sequence, wl1_) || (f.sequence == wl1_ && !before(f.acknowledgment, wl2_))))
        { peer_window_ = f.window; wl1_ = f.sequence; wl2_ = f.acknowledgment; }
        if (peer_window_) persist_deadline_ = 0;
        // RFC 5681 section 2: a duplicate ACK carries no data, SYN or FIN,
        // repeats SND.UNA with data outstanding and leaves the window alone.
        if (f.acknowledgment == previous_una && snd_una_ == previous_una && seg.payload.empty() &&
            !(f.flags & (tcp_syn | tcp_fin)) && f.window == previous_window && after(snd_max_, snd_una_) && sent_data())
            duplicate_ack();
        if (state_ == State::time_wait) return Ingress::ignored;
        consume(seg, now); return Ingress::accepted;
    }

  public:
    Status tick(u64 now)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before timer work");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        last_now_ = now;
        if (state_ == State::time_wait && now >= time_wait_deadline_)
        { state_ = State::closed; pending_ack_ = false; reset_pending_ = false; }
        if (rtx_.active && now >= rtx_.deadline && rtx_.retries >= max_retransmissions)
        {
            if (state_ == State::syn_received && passive_) return_to_listen();
            else terminate(Failure::timed_out);
        }
        if (state_ == State::fin_wait_2 && now >= fin_wait_deadline_) terminate(Failure::timed_out);
        return Status::success();
    }

    Result<std::optional<PreparedPacket>> prepare(std::span<std::byte> output, u64 now, u16 identification = 1)
    {
        if (plan_.active) return Status::busy("TCP packet token is outstanding");
        if (aliases_self(Bytes{output.data(), output.size()})) return Status::invalid_argument("TCP packet output aliases control-block storage");
        const auto status = tick(now); if (!status) return status;
        if (next_token_ == std::numeric_limits<u64>::max()) return Status::overflow("TCP packet token exhausted");
        Plan next{}; next.active = true; next.destination = peer_;
        const bool syn_outstanding = state_ == State::syn_sent || state_ == State::syn_received;
        const u8 syn_flags = static_cast<u8>(tcp_syn | (state_ == State::syn_received ? tcp_ack : 0));
        if (reset_pending_)
        { next.kind = Kind::reset; next.destination = reset_peer_; next.fields.sequence = reset_seq_; next.fields.flags = tcp_rst; }
        else if (pending_syn_)
        { next.kind = (rtx_.active && passive_) ? Kind::retry : Kind::syn; next.fields.sequence = iss_; next.fields.flags = syn_flags; }
        else if (rtx_.active && now >= rtx_.deadline)
        {
            next.kind = Kind::retry; next.fields.sequence = snd_una_;
            if (syn_outstanding) next.fields.flags = syn_flags;
            else if (!head(next)) return std::optional<PreparedPacket>{};
        }
        else if (retransmit_head_ && sending())
        {
            next.kind = Kind::fast; next.fields.sequence = snd_una_;
            if (!head(next)) { retransmit_head_ = false; return std::optional<PreparedPacket>{}; }
        }
        else if (!sending() || !send_next(next, now))
        {
            if (!pending_ack_) return std::optional<PreparedPacket>{};
            next.kind = Kind::ack; next.fields.sequence = snd_nxt_; next.fields.flags = tcp_ack;
        }
        next.fields.source_port = local_.port; next.fields.destination_port = next.destination.port;
        next.fields.acknowledgment = (next.fields.flags & tcp_ack) ? rcv_nxt_ : 0; next.fields.window = window();
        std::array<std::byte, SegmentCapacity> payload{};
        for (usize i = 0; i < next.payload; ++i) payload[i] = tx_[(tx_head_ + next.offset + i) % SendCapacity];
        const std::array<std::byte, 4> mss{std::byte{2}, std::byte{4},
            std::byte{static_cast<u8>(SegmentCapacity >> 8)}, std::byte{static_cast<u8>(SegmentCapacity)}};
        const Bytes options = (next.fields.flags & tcp_syn) ? Bytes{mss} : Bytes{}, data{payload.data(), next.payload};
        auto built = local_.address.v6()
            ? wire::build_tcp<wire::Ipv6Family>(output, local_.address.ipv6(), next.destination.address.ipv6(), identification,
                                                64, next.fields, options, data)
            : wire::build_tcp(output, local_.address.ipv4(), next.destination.address.ipv4(), identification, 64,
                              next.fields, options, data);
        if (!built) return built.status();
        next.token = ++next_token_; plan_ = next;
        return std::optional<PreparedPacket>{PreparedPacket{built.value(), plan_.token}};
    }
    Status complete_transmit(u64 token, bool accepted, u64 now)
    {
        if (!plan_.active || plan_.token != token) return Status::invalid_argument("stale TCP packet token");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        const Plan sent = plan_; plan_ = {}; last_now_ = now;
        if (!accepted) return Status::success();
        if (sent.kind == Kind::reset) { reset_pending_ = false; return Status::success(); }
        if (sent.fields.flags & tcp_ack) pending_ack_ = false;
        if (sent.kind == Kind::ack) return Status::success();
        const u32 end = sent.fields.sequence + static_cast<u32>(sent.payload) + ((sent.fields.flags & (tcp_syn | tcp_fin)) ? 1u : 0u);
        if (sent.kind == Kind::syn)
        {
            if (snd_max_ == iss_) { snd_nxt_ = snd_max_ = end; time(end, now); arm(now); }
            // Simultaneous open adds ACK to the retained initial SYN. Its timer
            // is restarted to avoid retrying less than one RTO after this send.
            // This ambiguous RTT sample is excluded by Karn's algorithm.
            else { rtt_timing_ = false; rtx_.deadline = deadline(now, rto_); }
            pending_syn_ = false; return Status::success();
        }
        if (sent.kind == Kind::retry)
        {
            ++rtx_.retries; rtt_timing_ = false;
            rto_ = min64(rto_ * 2, max_rto_ms); rtx_.deadline = deadline(now, rto_);
            if (sent.fields.flags & tcp_syn) { syn_retried_ = true; pending_syn_ = false; return Status::success(); }
            // RFC 5681 section 3.1 and RFC 6582 section 3.2 step 4: collapse the
            // window, end recovery and resend everything after this segment. A
            // probe of a zero window is not a loss and keeps the window.
            if (peer_window_) cc_.timeout(outstanding(), segment());
            recover_ = snd_max_; recovery_ = retransmit_head_ = false; dupacks_ = 0;
            snd_nxt_ = end;
            return Status::success();
        }
        if (sent.kind == Kind::fast) { retransmit_head_ = false; rtt_timing_ = false; return Status::success(); }
        const bool fresh = !before(sent.fields.sequence, snd_max_);
        snd_nxt_ = end; if (after(end, snd_max_)) snd_max_ = end;
        if (fresh) time(end, now);
        arm(now); persist_deadline_ = 0;
        if (sent.kind == Kind::fin && fresh) state_ = state_ == State::close_wait ? State::last_ack : State::fin_wait_1;
        return Status::success();
    }

  private:
    enum class Kind : u8 { ack, syn, data, fin, retry, reset, fast, probe };
    struct Timer { bool active{}; u64 deadline{}; u8 retries{}; };
    struct Plan { bool active{}; u64 token{}; Kind kind{}; Endpoint destination{}; TcpFields fields{}; usize payload{}, offset{}; };
    struct Range { u32 start{}, end{}; bool fin{}; };
    static constexpr usize min(usize a, usize b) { return a < b ? a : b; }
    static constexpr u64 min64(u64 a, u64 b) { return a < b ? a : b; }
    static constexpr u64 deadline(u64 now, u64 delay)
    { return now > std::numeric_limits<u64>::max() - delay ? std::numeric_limits<u64>::max() : now + delay; }
    static bool valid_endpoint(Endpoint ep) { return ep.port && ep.address.unicast(); }
    bool aliases_self(Bytes input) const
    {
        if (input.empty()) return false;
        const uptr destination = reinterpret_cast<uptr>(this), source = reinterpret_cast<uptr>(input.data());
        return source >= destination ? source - destination < sizeof(*this) : destination - source < input.size();
    }
    u16 window() const { return static_cast<u16>(ReceiveCapacity - rx_size_); }
    u32 segment() const { return static_cast<u32>(min(SegmentCapacity, peer_mss_)); }
    u32 outstanding() const { return snd_nxt_ - snd_una_; }
    // The FIN follows the last queued byte; it is in flight once SND.NXT passed it.
    u32 fin_sequence() const { return snd_una_ + static_cast<u32>(tx_size_); }
    bool fin_in_flight() const { return write_shutdown_ && snd_nxt_ == fin_sequence() + 1; }
    bool fin_sent() const { return write_shutdown_ && after(snd_max_, fin_sequence()); }
    // Queued bytes already sent at least once up to SND.NXT, from SND.UNA.
    usize sent_data() const { return outstanding() - (fin_in_flight() ? 1u : 0u); }
    bool sending() const
    {
        return state_ == State::established || state_ == State::close_wait || state_ == State::fin_wait_1 ||
            state_ == State::closing || state_ == State::last_ack;
    }
    usize out_of_order() const { usize n{}; for (usize i = 0; i < ooo_count_; ++i) n += ooo_[i].end - ooo_[i].start; return n; }
    void start_sequence(u32 iss) { iss_ = snd_una_ = snd_nxt_ = snd_max_ = recover_ = iss; }
    void time(u32 end, u64 now) { if (!rtt_timing_) { rtt_timing_ = true; rtt_sequence_ = end; rtt_sent_at_ = now; } }
    void arm(u64 now) { if (!rtx_.active) rtx_ = {true, deadline(now, rto_), 0}; }
    // The first unacknowledged segment: queued bytes from SND.UNA, or the FIN alone.
    bool head(Plan &next)
    {
        const usize data = min(min(tx_size_, usize{snd_max_ - snd_una_}), segment());
        if (data) { next.payload = data; next.offset = 0; next.fields.flags = tcp_ack | tcp_psh; return true; }
        if (fin_sent()) { next.fields.flags = tcp_fin | tcp_ack; return true; }
        return false;
    }
    // New data within min(peer window, congestion window), then the FIN, then
    // a one-byte probe of a zero window once the persist timer expired.
    bool send_next(Plan &next, u64 now)
    {
        const usize sent = sent_data(), unsent = tx_size_ > sent ? tx_size_ - sent : 0;
        const u32 limit = cc_.window() < peer_window_ ? cc_.window() : peer_window_;
        const u32 usable = limit > outstanding() ? limit - outstanding() : 0;
        next.fields.sequence = snd_nxt_;
        if (unsent && usable)
        {
            next.kind = Kind::data; next.fields.flags = tcp_ack | tcp_psh; next.offset = sent;
            next.payload = min(min(unsent, segment()), usable); return true;
        }
        if (unsent && !peer_window_ && !outstanding())
        {
            // RFC 9293 section 3.8.6.1: probe a zero window at the RTO, backing off.
            if (!persist_deadline_) { persist_deadline_ = deadline(now, rto_); return false; }
            if (now < persist_deadline_) return false;
            next.kind = Kind::probe; next.fields.flags = tcp_ack | tcp_psh; next.offset = sent; next.payload = 1; return true;
        }
        if (!unsent && write_shutdown_ && peer_window_ && snd_nxt_ == fin_sequence() && !fin_acknowledged_)
        { next.kind = Kind::fin; next.fields.flags = tcp_fin | tcp_ack; return true; }
        return false;
    }
    bool acceptable(u32 seq, u32 length) const
    {
        const u32 wnd = window();
        const auto in = [this, wnd](u32 value) { return u32{value - rcv_nxt_} < wnd; };
        if (!length) return wnd ? in(seq) : seq == rcv_nxt_;
        return wnd && (in(seq) || in(seq + length - 1));
    }
    Ingress challenge(u64 now)
    {
        // Per-connection bounded challenge rate; executor must add global limits.
        if (!challenge_seen_ || now >= challenge_deadline_)
        { pending_ack_ = true; challenge_seen_ = true; challenge_deadline_ = deadline(now, 1000); }
        return Ingress::challenged;
    }
    void queue_reset(Endpoint destination, u32 seq)
    { reset_pending_ = true; reset_peer_ = destination; reset_seq_ = seq; }
    void negotiate_mss(const wire::Tcp &seg)
    {
        peer_mss_ = 536; wire::TcpOptions cursor{seg.options, seg.fields.flags};
        for (;;)
        {
            auto option = cursor.next(); if (!option || !option.value()) break;
            if (option.value()->kind == 2)
            { const u16 value = wire::be16(option.value()->data, 0); if (value) { peer_mss_ = value; break; } }
        }
        cc_.start(segment());
    }
    Ingress receive_syn(const wire::Tcp &seg, u64 now)
    {
        const auto &f = seg.fields;
        if (state_ == State::syn_received && !passive_ && f.sequence == irs_ && (f.flags & tcp_ack) &&
            !(f.flags & tcp_fin) && f.acknowledgment == snd_max_ && snd_max_ == iss_ + 1)
        {
            acknowledge(f.acknowledgment, now); state_ = State::established; pending_syn_ = false;
            peer_window_ = f.window; wl1_ = f.sequence; wl2_ = f.acknowledgment;
            pending_ack_ = true; if (syn_retried_ && rto_ < 3000) rto_ = 3000;
            return Ingress::accepted;
        }
        return challenge(now);
    }
    void sample_rtt(u64 sample)
    {
        // RFC 6298: G=1 ms; fixed-point SRTT/RTTVAR, variance updated first.
        sample = min64(sample, max_rto_ms * 1024);
        if (!rtt_known_) { srtt8_ = sample * 8; variance4_ = sample * 2; rtt_known_ = true; }
        else
        {
            const u64 old_srtt = srtt8_ / 8;
            const u64 error = old_srtt > sample ? old_srtt - sample : sample - old_srtt;
            variance4_ = (3 * variance4_ + 4 * error) / 4;
            srtt8_ = (7 * srtt8_ + sample * 8) / 8;
        }
        const u64 computed = (srtt8_ + 7) / 8 + (variance4_ ? variance4_ : 1);
        rto_ = min64(computed < 1000 ? 1000 : computed, max_rto_ms);
    }
    void acknowledge(u32 ack, u64 now)
    {
        if (!after(ack, snd_una_) || after(ack, snd_max_)) return;
        const bool syn = state_ == State::syn_sent || state_ == State::syn_received;
        const bool fin = fin_sent() && ack == fin_sequence() + 1;
        const u32 data = (ack - snd_una_) - (syn ? 1u : 0u) - (fin ? 1u : 0u);
        const usize released = min(data, tx_size_);
        tx_head_ = (tx_head_ + released) % SendCapacity; tx_size_ -= released;
        if (rtt_timing_ && !before(ack, rtt_sequence_)) { sample_rtt(now - rtt_sent_at_); rtt_timing_ = false; }
        snd_una_ = ack; if (before(snd_nxt_, snd_una_)) snd_nxt_ = snd_una_;
        if (fin) fin_acknowledged_ = true;
        dupacks_ = 0;
        if (recovery_)
        {
            // RFC 6582 section 3.2 steps 5 and 6.
            if (!before(ack, recover_)) { recovery_ = false; cc_.exit_recovery(snd_max_ - snd_una_, segment()); }
            else { cc_.partial(static_cast<u32>(released), segment()); retransmit_head_ = true; }
        }
        else if (released) cc_.acknowledged(static_cast<u32>(released), segment());
        rtx_.retries = 0;
        if (snd_una_ == snd_max_) rtx_ = {};
        else rtx_.deadline = deadline(now, rto_);
        if (fin)
        {
            if (state_ == State::fin_wait_1) { state_ = State::fin_wait_2; fin_wait_deadline_ = deadline(now, 2 * msl_ms); }
            else if (state_ == State::closing) enter_time_wait(now);
            else if (state_ == State::last_ack) { state_ = State::closed; pending_ack_ = false; }
        }
    }
    void duplicate_ack()
    {
        if (dupacks_ != 0xff) ++dupacks_;
        if (recovery_) { cc_.duplicate(segment()); return; }
        // RFC 6582 section 3.2 step 2: one recovery per window of data.
        if (dupacks_ == fast_retransmit_threshold && after(snd_una_, recover_))
        {
            cc_.enter_recovery(snd_max_ - snd_una_, segment());
            recover_ = snd_max_; recovery_ = true; retransmit_head_ = true;
        }
    }
    void consume(const wire::Tcp &seg, u64 now)
    {
        const auto &f = seg.fields;
        const bool fin = (f.flags & tcp_fin) != 0;
        if (seg.payload.empty() && !fin) return;
        pending_ack_ = true;
        if (eof_) return;
        u32 sequence = f.sequence; Bytes data = seg.payload;
        if (before(sequence, rcv_nxt_))
        {
            const u32 skip = rcv_nxt_ - sequence;
            if (skip > data.size()) return;
            data = data.subspan(skip); sequence = rcv_nxt_;
        }
        const usize free = ReceiveCapacity - rx_size_, offset = u32{sequence - rcv_nxt_};
        if (offset >= free) return;
        const usize count = min(data.size(), free - offset);
        for (usize i = 0; i < count; ++i) rx_[(rx_head_ + rx_size_ + offset + i) % ReceiveCapacity] = data[i];
        const bool closes = fin && count == data.size();
        if (offset) { remember(sequence, sequence + static_cast<u32>(count), closes); return; }
        rx_size_ += count; rcv_nxt_ += static_cast<u32>(count);
        bool final = closes;
        // Join every kept segment the new bytes reached.
        for (bool joined = true; joined;)
        {
            joined = false;
            for (usize i = 0; i < ooo_count_; ++i)
            {
                const Range range = ooo_[i];
                if (after(range.start, rcv_nxt_)) continue;
                if (after(range.end, rcv_nxt_)) { rx_size_ += range.end - rcv_nxt_; rcv_nxt_ = range.end; final = false; }
                if (range.fin && range.end == rcv_nxt_) final = true;
                ooo_[i] = ooo_[--ooo_count_]; joined = true; break;
            }
        }
        if (final && window())
        {
            ++rcv_nxt_; eof_ = true; ooo_count_ = 0;
            if (state_ == State::established) state_ = State::close_wait;
            else if (state_ == State::fin_wait_1) state_ = State::closing;
            else if (state_ == State::fin_wait_2) enter_time_wait(now);
        }
    }
    // Keeps an out-of-order range, merged with those it overlaps or touches.
    // Without a free record the bytes stay unclaimed and are resent by the peer.
    void remember(u32 start, u32 end, bool fin)
    {
        if (start == end && !fin) return;
        for (usize i = 0; i < ooo_count_;)
        {
            const Range r = ooo_[i];
            if (before(end, r.start) || before(r.end, start)) { ++i; continue; }
            const u32 first = before(r.start, start) ? r.start : start, last = after(r.end, end) ? r.end : end;
            fin = (fin && end == last) || (r.fin && r.end == last);
            start = first; end = last; ooo_[i] = ooo_[--ooo_count_];
        }
        if (ooo_count_ < max_out_of_order) ooo_[ooo_count_++] = {start, end, fin};
    }
    void enter_time_wait(u64 now)
    { state_ = State::time_wait; time_wait_deadline_ = deadline(now, 2 * msl_ms); rtx_ = {}; }
    void terminate(Failure reason)
    {
        state_ = State::closed; failure_ = reason; tx_size_ = rx_size_ = 0; ooo_count_ = 0;
        rtx_ = {}; pending_syn_ = pending_ack_ = reset_pending_ = retransmit_head_ = false; write_shutdown_ = true;
    }
    void return_to_listen()
    {
        const Endpoint saved = local_; const u32 iss = iss_; reset_fields(); local_ = saved;
        start_sequence(iss); state_ = State::listen; passive_ = true;
    }
    void reset_fields()
    {
        state_ = State::closed; failure_ = Failure::none; peer_ = {}; tx_head_ = tx_size_ = rx_head_ = rx_size_ = 0;
        snd_una_ = snd_nxt_ = snd_max_ = rcv_nxt_ = irs_ = iss_ = wl1_ = wl2_ = recover_ = 0; peer_window_ = 0; peer_mss_ = 536;
        rtx_ = {}; plan_ = {}; rto_ = 1000; srtt8_ = variance4_ = time_wait_deadline_ = fin_wait_deadline_ = 0;
        passive_ = pending_syn_ = pending_ack_ = reset_pending_ = eof_ = write_shutdown_ = rtt_known_ = syn_retried_ = false;
        challenge_seen_ = listener_armed_ = rtt_timing_ = recovery_ = retransmit_head_ = fin_acknowledged_ = false;
        challenge_deadline_ = persist_deadline_ = rtt_sent_at_ = 0; rtt_sequence_ = 0; dupacks_ = 0; ooo_count_ = 0;
        cc_ = {}; cc_.start(segment());
        // Packet token generation and monotonic time survive control-block reuse.
    }
    State state_{State::closed}; Failure failure_{Failure::none}; Endpoint local_{}, peer_{}, reset_peer_{};
    std::array<std::byte, SendCapacity> tx_{}; std::array<std::byte, ReceiveCapacity> rx_{};
    usize tx_head_{}, tx_size_{}, rx_head_{}, rx_size_{};
    u32 snd_una_{}, snd_nxt_{}, snd_max_{}, rcv_nxt_{}, iss_{}, irs_{}, wl1_{}, wl2_{}, reset_seq_{}, recover_{}, rtt_sequence_{};
    u16 peer_window_{}, peer_mss_{536}; Timer rtx_{}; Plan plan_{};
    u64 rto_{1000}, srtt8_{}, variance4_{}, last_now_{}, next_token_{}, time_wait_deadline_{}, fin_wait_deadline_{}, challenge_deadline_{};
    u64 persist_deadline_{}, rtt_sent_at_{};
    bool passive_{}, pending_syn_{}, pending_ack_{}, reset_pending_{}, eof_{}, write_shutdown_{}, rtt_known_{}, syn_retried_{}, challenge_seen_{}, listener_armed_{};
    bool rtt_timing_{}, recovery_{}, retransmit_head_{}, fin_acknowledged_{};
    u8 dupacks_{};
    std::array<Range, max_out_of_order> ooo_{}; usize ooo_count_{};
    Congestion cc_{};
};
}
