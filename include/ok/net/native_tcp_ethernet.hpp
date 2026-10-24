#pragma once
#include <ok/net/tcp_stream_wait.hpp>
#include <ok/net/native_udp_executor.hpp>

namespace ok::net
{
struct TcpEthernetState
{
    bool retired{}; usize credit{}, controls{};
    u64 received{}, tcp_received{}, udp_received{}, submitted{}, dropped{}, dropped_control{};
    StatusCode cause{StatusCode::ok};
};

// Sole copied RX consumer for this static peer link. Do not run the existing
// UDP executor against the same RX queue. OS owns the actual Ethernet/DMA driver.
class NativeTcpEthernetExecutor final
{
  public:
    static constexpr usize budget = 8, maximum_credit = 8, control_capacity = 2;
    NativeTcpEthernetExecutor() = default;
    NativeTcpEthernetExecutor(const NativeTcpEthernetExecutor &) = delete;
    NativeTcpEthernetExecutor &operator=(const NativeTcpEthernetExecutor &) = delete;
    NativeTcpEthernetExecutor(NativeTcpEthernetExecutor &&) = delete;
    NativeTcpEthernetExecutor &operator=(NativeTcpEthernetExecutor &&) = delete;
    [[nodiscard]] TcpEthernetState state() const
    { return {retired_, credit_, count_, received_, tcp_received_, udp_received_, submitted_, dropped_, dropped_control_, cause_}; }
    Status retire(NativeTcpStreamService &tcp, TcpStreamWaits &waits, Status cause)
    {
        const auto drained = tcp.endpoints().retire(); if (!drained) return drained;
        retired_ = true; credit_ = count_ = 0; cause_ = cause.code();
        const auto wake = waits.poll(); if (!wake) return wake;
        return Status::io_error("TCP Ethernet executor is retired");
    }
    template <NativeUdpTransport Transport, tcp::IssSource Source>
    Status poll(NativeTcpStreamService &tcp, NativeUdpService &udp, TcpStreamWaits &waits,
                Transport &transport, Source &iss, u64 now)
    {
        if (retired_) return Status::io_error("TCP Ethernet executor is retired");
        if (!udp.configured()) return Status::not_initialized("TCP Ethernet static link is unconfigured");
        if (tcp.endpoints().pools().prepared) return Status::busy("TCP external packet ticket must complete first");
        if (auto drained = tcp.drain_closes(); !drained) return drained;
        if (auto timed = tcp.endpoints().tick(now); !timed) return timed;
        auto reaped = transport.reap(budget); if (!reaped) return retire(tcp, waits, reaped.status());
        if (reaped.value().completed > budget || reaped.value().credit > maximum_credit)
            return retire(tcp, waits, Status::io_error("Ethernet TX ledger exceeds its contract"));
        credit_ = reaped.value().credit;
        const auto &link = udp.link();
        for (usize i = 0; i < budget; ++i)
        {
            auto received = transport.receive(incoming_);
            if (!received)
            {
                if (received.status().code() == StatusCode::not_found || received.status().code() == StatusCode::would_block) break;
                return retire(tcp, waits, received.status());
            }
            if (!received.value() || received.value() > incoming_.size())
                return retire(tcp, waits, Status::io_error("Ethernet copied RX length is invalid"));
            add(received_); const wire::Bytes frame{incoming_.data(), received.value()};
            auto ethernet = wire::ethernet(frame);
            if (!ethernet) { add(dropped_); continue; }
            const auto &eth = ethernet.value();
            if (eth.type == 0x0800)
            {
                auto ip = wire::ipv4(eth.payload);
                if (!ip || eth.destination != link.local_mac || eth.source != link.peer_mac ||
                    ip.value().source != link.peer_ip || ip.value().destination != link.local_ip)
                { add(dropped_); continue; }
                if (ip.value().protocol == 6)
                {
                    const usize length = wire::be16(eth.payload, 2);
                    auto accepted = tcp.endpoints().ingest(eth.payload.first(length), now, iss);
                    if (!accepted || accepted.value().dropped) add(dropped_);
                    else add(tcp_received_);
                    continue;
                }
                if (ip.value().protocol != 17) { add(dropped_); continue; }
            }
            else if (eth.type != 0x0806) { add(dropped_); continue; }
            auto accepted = udp.ingest(frame, reply_);
            if (!accepted)
            {
                if (!udp.status()) return retire(tcp, waits, udp.status());
                add(dropped_); continue;
            }
            if (accepted.value().kind == NativeUdpFrameKind::udp_queued) add(udp_received_);
            else if (accepted.value().kind == NativeUdpFrameKind::arp_reply)
            {
                const usize length = accepted.value().reply_length;
                if (!length || length > reply_.size()) return retire(tcp, waits, Status::io_error("ARP reply length is invalid"));
                if (count_ == control_capacity) { add(dropped_); add(dropped_control_); continue; }
                auto &control = controls_[(head_ + count_) % control_capacity]; control.length = length;
                for (usize byte = 0; byte < length; ++byte) control.bytes[byte] = reply_[byte];
                ++count_;
            }
            else add(dropped_);
        }
        if (auto drained = tcp.drain_closes(); !drained) return retire(tcp, waits, drained);
        for (usize i = 0; i < budget && credit_; ++i)
        {
            if (count_)
            {
                const auto &control = controls_[head_];
                auto copied = transport.try_transmit({control.bytes.data(), control.length});
                if (!copied)
                {
                    if (copied.code() == StatusCode::would_block) { credit_ = 0; break; }
                    return retire(tcp, waits, copied);
                }
                head_ = (head_ + 1) % control_capacity; --count_; --credit_; add(submitted_); continue;
            }
            auto prepared = tcp.endpoints().prepare(std::span{outgoing_}.subspan(14), now);
            if (!prepared) return retire(tcp, waits, prepared.status());
            if (!prepared.value()) break;
            const auto packet = *prepared.value();
            const wire::Bytes bytes{outgoing_.data() + 14, packet.length};
            auto ip = wire::ipv4(bytes);
            if (!ip || ip.value().source != link.local_ip || ip.value().destination != link.peer_ip)
            {
                static_cast<void>(tcp.endpoints().complete(packet.ticket, false, now));
                return retire(tcp, waits, Status::unsupported("TCP destination has no static peer route"));
            }
            for (usize byte = 0; byte < 6; ++byte)
            { outgoing_[byte] = std::byte{link.peer_mac[byte]}; outgoing_[byte + 6] = std::byte{link.local_mac[byte]}; }
            wire::be16(outgoing_, 12, 0x0800);
            auto copied = transport.try_transmit({outgoing_.data(), 14 + packet.length});
            auto committed = tcp.endpoints().complete(packet.ticket, bool(copied), now);
            if (!committed) return retire(tcp, waits, committed);
            if (!copied)
            {
                if (copied.code() == StatusCode::would_block) { credit_ = 0; break; }
                return retire(tcp, waits, copied);
            }
            --credit_; add(submitted_);
        }
        return waits.poll();
    }
  private:
    static void add(u64 &count) { if (count != ~u64{0}) ++count; }
    struct Control { usize length{}; std::array<std::byte, 1514> bytes{}; };
    std::array<std::byte, 1514> incoming_{}, outgoing_{}, reply_{};
    std::array<Control, control_capacity> controls_{};
    usize head_{}, count_{}, credit_{};
    u64 received_{}, tcp_received_{}, udp_received_{}, submitted_{}, dropped_{}, dropped_control_{};
    bool retired_{}; StatusCode cause_{StatusCode::ok};
};
}
