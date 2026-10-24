#pragma once

#include <ok/net/tcp_wire.hpp>
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

struct Endpoint { Ipv4 address{}; u16 port{}; bool operator==(const Endpoint &) const = default; };
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
};

// One executor owns this noncopyable control block. Input and write bytes are
// copied before return; no wire/user span is retained. Each prepare grants one
// packet token, which must be completed before another state-changing operation.
// Caller owns emitted bytes; accepted device TX never releases the retained
// stream bytes. Only a validated peer ACK releases retransmittable storage.
// This deliberately conservative first core keeps only one segment in flight.
template <usize SendCapacity = 4096, usize ReceiveCapacity = 4096, usize SegmentCapacity = 536>
class Connection final
{
    static_assert(SendCapacity > 0 && SendCapacity <= 65535);
    static_assert(ReceiveCapacity > 0 && ReceiveCapacity <= 65535);
    static_assert(SegmentCapacity > 0 && SegmentCapacity <= 1460);
  public:
    static constexpr u64 msl_ms = 120000, max_rto_ms = 60000;
    static constexpr u8 max_retransmissions = 8;
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;

    [[nodiscard]] Snapshot snapshot() const
    {
        return {state_, failure_, snd_una_, snd_nxt_, rcv_nxt_, tx_size_, rx_size_, peer_window_,
            window(), peer_mss_, flight_.active ? flight_.deadline : 0, time_wait_deadline_, rto_,
            flight_.retries, flight_.active, eof_, write_shutdown_, plan_.active, listener_armed_};
    }
    [[nodiscard]] Endpoint local() const { return local_; }
    [[nodiscard]] Endpoint peer() const { return peer_; }

    // ISS is supplied by the caller. This core provides no entropy and must not
    // be exposed to an untrusted network with a predictable sequence source.
    Status active_open(Endpoint local, Endpoint peer, u32 iss, u64 now)
    {
        if (plan_.active || state_ != State::closed || rx_size_) return Status::busy("TCP control block is in use");
        if (!valid_endpoint(local) || !valid_endpoint(peer)) return Status::invalid_argument("invalid TCP endpoints");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        reset_fields(); local_ = local; peer_ = peer; iss_ = snd_una_ = snd_nxt_ = iss;
        state_ = State::syn_sent; pending_syn_ = true; last_now_ = now;
        return Status::success();
    }
    Status passive_open(Endpoint local, u32 iss, u64 now)
    {
        if (plan_.active || state_ != State::closed || rx_size_) return Status::busy("TCP control block is in use");
        if (!valid_endpoint(local)) return Status::invalid_argument("invalid TCP listener endpoint");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        reset_fields(); local_ = local; iss_ = snd_una_ = snd_nxt_ = iss;
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
        iss_ = snd_una_ = snd_nxt_ = iss; listener_armed_ = true; last_now_ = now;
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

    // This accepts complete IPv4 bytes, not caller-forged parsed fields. The
    // unchanged codecs validate IP and mandatory TCP checksums before any state.
    Result<Ingress> receive(Bytes packet, u64 now)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before ingress");
        if (aliases_self(packet)) return Status::invalid_argument("TCP ingress aliases control-block storage");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        auto ip = wire::ipv4(packet); if (!ip) return ip.status();
        auto tcp = wire::tcp(ip.value()); if (!tcp) return tcp.status();
        const auto &seg = tcp.value(); const auto &f = seg.fields;
        if (ip.value().destination != local_.address || f.destination_port != local_.port || !f.source_port)
            return Ingress::ignored;
        const Endpoint source{ip.value().source, f.source_port};
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
            if (ack && (f.acknowledgment != iss_ + 1 || snd_nxt_ != iss_ + 1))
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
            if (!between(f.acknowledgment, snd_una_ - 65535u, snd_nxt_)) return challenge(now);
            pending_ack_ = true; time_wait_deadline_ = deadline(now, 2 * msl_ms); return Ingress::accepted;
        }
        if (state_ == State::syn_received && passive_ && f.sequence == irs_ &&
            (f.flags & tcp_syn) && !(f.flags & (tcp_ack | tcp_rst | tcp_fin)))
        {
            // A lost SYN-ACK must not elicit an ACK-only response: SYN-SENT
            // rejects it. Retain the original SYN sequence, and respect RTO.
            if (!flight_.active || now >= flight_.deadline) pending_syn_ = true;
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
        if (!between(f.acknowledgment, snd_una_ - 65535u, snd_nxt_)) return challenge(now);
        if (state_ == State::syn_received)
        {
            if (f.acknowledgment != snd_nxt_ || snd_nxt_ != iss_ + 1)
            { queue_reset(peer_, f.acknowledgment); return Ingress::ignored; }
            acknowledge(f.acknowledgment, now); state_ = State::established; pending_syn_ = false;
            if (syn_retried_ && rto_ < 3000) rto_ = 3000;
        }
        else if (after(f.acknowledgment, snd_una_)) acknowledge(f.acknowledgment, now);
        if (state_ == State::closed) return Ingress::accepted;
        if (between(f.acknowledgment, snd_una_, snd_nxt_) &&
            (after(f.sequence, wl1_) || (f.sequence == wl1_ && !before(f.acknowledgment, wl2_))))
        { peer_window_ = f.window; wl1_ = f.sequence; wl2_ = f.acknowledgment; }
        if (state_ == State::time_wait) return Ingress::ignored;
        consume(seg, now); return Ingress::accepted;
    }

    Status tick(u64 now)
    {
        if (plan_.active) return Status::busy("complete prepared TCP packet before timer work");
        if (now < last_now_) return Status::invalid_argument("TCP clock moved backward");
        last_now_ = now;
        if (state_ == State::time_wait && now >= time_wait_deadline_)
        { state_ = State::closed; pending_ack_ = false; reset_pending_ = false; }
        if (flight_.active && now >= flight_.deadline && flight_.retries >= max_retransmissions)
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
        if (reset_pending_)
        { next.kind = Kind::reset; next.destination = reset_peer_; next.fields.sequence = reset_seq_; next.fields.flags = tcp_rst; }
        else if (pending_syn_)
        { next.kind = (flight_.active && passive_) ? Kind::retry : Kind::syn; next.fields.sequence = iss_; next.fields.flags = tcp_syn | (state_ == State::syn_received ? tcp_ack : 0); }
        else if (flight_.active && now >= flight_.deadline)
        { next.kind = Kind::retry; next.fields.sequence = flight_.sequence; next.fields.flags = flight_.flags; next.payload = flight_.payload; }
        else if (pending_ack_)
        { next.kind = Kind::ack; next.fields.sequence = snd_nxt_; next.fields.flags = tcp_ack; }
        else if (!flight_.active && (state_ == State::established || state_ == State::close_wait))
        {
            if (tx_size_ && peer_window_)
            { next.kind = Kind::data; next.fields.sequence = snd_nxt_; next.fields.flags = tcp_ack | tcp_psh;
                next.payload = min(min(tx_size_, SegmentCapacity), min(usize{peer_mss_}, usize{peer_window_})); }
            else if (!tx_size_ && write_shutdown_ && peer_window_)
            { next.kind = Kind::fin; next.fields.sequence = snd_nxt_; next.fields.flags = tcp_fin | tcp_ack; }
            else return std::optional<PreparedPacket>{};
        }
        else return std::optional<PreparedPacket>{};
        next.fields.source_port = local_.port; next.fields.destination_port = next.destination.port;
        next.fields.acknowledgment = (next.fields.flags & tcp_ack) ? rcv_nxt_ : 0; next.fields.window = window();
        std::array<std::byte, SegmentCapacity> payload{};
        for (usize i = 0; i < next.payload; ++i) payload[i] = tx_[(tx_head_ + i) % SendCapacity];
        const std::array<std::byte, 4> mss{std::byte{2}, std::byte{4},
            std::byte{static_cast<u8>(SegmentCapacity >> 8)}, std::byte{static_cast<u8>(SegmentCapacity)}};
        auto built = wire::build_tcp(output, local_.address, next.destination.address, identification, 64,
            next.fields, (next.fields.flags & tcp_syn) ? Bytes{mss} : Bytes{}, Bytes{payload.data(), next.payload});
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
        if (sent.kind == Kind::retry)
        {
            ++flight_.retries; flight_.retransmitted = true;
            if (flight_.flags & tcp_syn) { syn_retried_ = true; pending_syn_ = false; }
            rto_ = min64(rto_ * 2, max_rto_ms); flight_.deadline = deadline(now, rto_);
            return Status::success();
        }
        if (sent.kind == Kind::syn && flight_.active)
        {
            // Simultaneous open adds ACK to the retained initial SYN. Its timer
            // is restarted to avoid retrying less than one RTO after this send.
            // This ambiguous RTT sample is excluded by Karn's algorithm.
            flight_.flags = sent.fields.flags; flight_.retransmitted = true; flight_.deadline = deadline(now, rto_); pending_syn_ = false;
            return Status::success();
        }
        flight_ = {true, sent.fields.sequence, sent.fields.flags, sent.payload, now, deadline(now, rto_), 0, false};
        snd_nxt_ += static_cast<u32>(sent.payload) + ((sent.fields.flags & (tcp_syn | tcp_fin)) ? 1u : 0u);
        if (sent.kind == Kind::syn) pending_syn_ = false;
        if (sent.kind == Kind::fin) state_ = state_ == State::close_wait ? State::last_ack : State::fin_wait_1;
        return Status::success();
    }

  private:
    enum class Kind : u8 { ack, syn, data, fin, retry, reset };
    struct Flight { bool active{}; u32 sequence{}; u8 flags{}; usize payload{}; u64 sent_at{}, deadline{}; u8 retries{}; bool retransmitted{}; };
    struct Plan { bool active{}; u64 token{}; Kind kind{}; Endpoint destination{}; TcpFields fields{}; usize payload{}; };
    static constexpr usize min(usize a, usize b) { return a < b ? a : b; }
    static constexpr u64 min64(u64 a, u64 b) { return a < b ? a : b; }
    static constexpr u64 deadline(u64 now, u64 delay)
    { return now > std::numeric_limits<u64>::max() - delay ? std::numeric_limits<u64>::max() : now + delay; }
    static bool valid_endpoint(Endpoint ep)
    { return ep.port && ep.address != Ipv4{} && ep.address[0] < 224 && ep.address[0] != 0; }
    bool aliases_self(Bytes input) const
    {
        if (input.empty()) return false;
        const uptr destination = reinterpret_cast<uptr>(this), source = reinterpret_cast<uptr>(input.data());
        return source >= destination ? source - destination < sizeof(*this) : destination - source < input.size();
    }
    u16 window() const { return static_cast<u16>(ReceiveCapacity - rx_size_); }
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
    }
    Ingress receive_syn(const wire::Tcp &seg, u64 now)
    {
        const auto &f = seg.fields;
        if (state_ == State::syn_received && !passive_ && f.sequence == irs_ && (f.flags & tcp_ack) &&
            !(f.flags & tcp_fin) && f.acknowledgment == snd_nxt_ && snd_nxt_ == iss_ + 1)
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
        if (!flight_.active || !after(ack, snd_una_) || after(ack, snd_nxt_)) return;
        const u32 advance = ack - snd_una_; const bool complete = ack == snd_nxt_;
        if (complete && !flight_.retransmitted) sample_rtt(now - flight_.sent_at);
        if (flight_.payload)
        {
            const usize count = min(advance, flight_.payload);
            tx_head_ = (tx_head_ + count) % SendCapacity; tx_size_ -= count; flight_.payload -= count;
        }
        snd_una_ = ack; flight_.sequence = ack;
        if (!complete) { flight_.retransmitted = true; flight_.deadline = deadline(now, rto_); return; }
        const bool fin = (flight_.flags & tcp_fin) != 0; flight_ = {};
        if (fin)
        {
            if (state_ == State::fin_wait_1) { state_ = State::fin_wait_2; fin_wait_deadline_ = deadline(now, 2 * msl_ms); }
            else if (state_ == State::closing) enter_time_wait(now);
            else if (state_ == State::last_ack) { state_ = State::closed; pending_ack_ = false; }
        }
    }
    void consume(const wire::Tcp &seg, u64 now)
    {
        const auto &f = seg.fields;
        if (seg.payload.empty() && !(f.flags & tcp_fin)) return;
        pending_ack_ = true;
        if (eof_ || after(f.sequence, rcv_nxt_)) return;
        const usize skip = static_cast<u32>(rcv_nxt_ - f.sequence);
        if (skip > seg.payload.size()) return;
        const usize count = min(seg.payload.size() - skip, ReceiveCapacity - rx_size_);
        for (usize i = 0; i < count; ++i) rx_[(rx_head_ + rx_size_ + i) % ReceiveCapacity] = seg.payload[skip + i];
        rx_size_ += count; rcv_nxt_ += static_cast<u32>(count);
        if ((f.flags & tcp_fin) && f.sequence + static_cast<u32>(seg.payload.size()) == rcv_nxt_ && window())
        {
            ++rcv_nxt_; eof_ = true;
            if (state_ == State::established) state_ = State::close_wait;
            else if (state_ == State::fin_wait_1) state_ = State::closing;
            else if (state_ == State::fin_wait_2) enter_time_wait(now);
        }
    }
    void enter_time_wait(u64 now)
    { state_ = State::time_wait; time_wait_deadline_ = deadline(now, 2 * msl_ms); flight_ = {}; }
    void terminate(Failure reason)
    {
        state_ = State::closed; failure_ = reason; tx_size_ = rx_size_ = 0;
        flight_ = {}; pending_syn_ = pending_ack_ = reset_pending_ = false; write_shutdown_ = true;
    }
    void return_to_listen()
    {
        const Endpoint saved = local_; const u32 iss = iss_; reset_fields(); local_ = saved;
        iss_ = snd_una_ = snd_nxt_ = iss; state_ = State::listen; passive_ = true;
    }
    void reset_fields()
    {
        state_ = State::closed; failure_ = Failure::none; peer_ = {}; tx_head_ = tx_size_ = rx_head_ = rx_size_ = 0;
        snd_una_ = snd_nxt_ = rcv_nxt_ = irs_ = iss_ = wl1_ = wl2_ = 0; peer_window_ = 0; peer_mss_ = 536;
        flight_ = {}; plan_ = {}; rto_ = 1000; srtt8_ = variance4_ = time_wait_deadline_ = fin_wait_deadline_ = 0;
        passive_ = pending_syn_ = pending_ack_ = reset_pending_ = eof_ = write_shutdown_ = rtt_known_ = syn_retried_ = false;
        challenge_seen_ = listener_armed_ = false; challenge_deadline_ = 0;
        // Packet token generation and monotonic time survive control-block reuse.
    }
    State state_{State::closed}; Failure failure_{Failure::none}; Endpoint local_{}, peer_{}, reset_peer_{};
    std::array<std::byte, SendCapacity> tx_{}; std::array<std::byte, ReceiveCapacity> rx_{};
    usize tx_head_{}, tx_size_{}, rx_head_{}, rx_size_{};
    u32 snd_una_{}, snd_nxt_{}, rcv_nxt_{}, iss_{}, irs_{}, wl1_{}, wl2_{}, reset_seq_{};
    u16 peer_window_{}, peer_mss_{536}; Flight flight_{}; Plan plan_{};
    u64 rto_{1000}, srtt8_{}, variance4_{}, last_now_{}, next_token_{}, time_wait_deadline_{}, fin_wait_deadline_{}, challenge_deadline_{};
    bool passive_{}, pending_syn_{}, pending_ack_{}, reset_pending_{}, eof_{}, write_shutdown_{}, rtt_known_{}, syn_retried_{}, challenge_seen_{}, listener_armed_{};
};
}
