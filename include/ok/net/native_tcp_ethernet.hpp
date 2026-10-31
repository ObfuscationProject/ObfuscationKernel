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
    // The last device pass stopped at its receive budget; frames may remain.
    bool receive_backlog{};
};

// What the kernel loop asks of one pass. Without a device event (the link's
// interrupt has not fired since the last pass) the TX ledger and the RX queue
// are left alone; closes, protocol timers and queued segments still run.
// `source` is credited to TCP waiters the pass's frames made ready.
struct LinkPass
{
    bool device{true};
    sched::WakeSource source{sched::WakeSource::poll};
};

// Sole copied RX consumer of the link. Do not run the standalone UDP executor
// against the same RX queue. OS owns the actual Ethernet/DMA driver; the UDP
// service's attached interface owns link control, routing and resolution.
class NativeTcpEthernetExecutor final
{
  public:
    static constexpr usize budget = 8, maximum_credit = 8, control_capacity = EthernetInterface::control_capacity;
    NativeTcpEthernetExecutor() = default;
    NativeTcpEthernetExecutor(const NativeTcpEthernetExecutor &) = delete;
    NativeTcpEthernetExecutor &operator=(const NativeTcpEthernetExecutor &) = delete;
    NativeTcpEthernetExecutor(NativeTcpEthernetExecutor &&) = delete;
    NativeTcpEthernetExecutor &operator=(NativeTcpEthernetExecutor &&) = delete;
    [[nodiscard]] TcpEthernetState state() const
    { return {retired_, credit_, controls_, received_, tcp_received_, udp_received_, submitted_, dropped_, dropped_control_, cause_, backlog_}; }
    Status retire(NativeTcpStreamService &tcp, TcpStreamWaits &waits, Status cause)
    {
        const auto drained = tcp.endpoints().retire(); if (!drained) return drained;
        retired_ = true; credit_ = controls_ = 0; cause_ = cause.code();
        const auto wake = waits.poll(); if (!wake) return wake;
        return Status::io_error("TCP Ethernet executor is retired");
    }
    template <NativeUdpTransport Transport, tcp::IssSource Source>
    Status poll(NativeTcpStreamService &tcp, NativeUdpService &udp, TcpStreamWaits &waits,
                Transport &transport, Source &iss, u64 now, LinkPass pass = {})
    {
        if (retired_) return Status::io_error("TCP Ethernet executor is retired");
        if (!udp.configured()) return Status::not_initialized("TCP Ethernet link is unconfigured");
        auto *nic = udp.interface();
        if (!nic || !nic->configured()) return Status::not_initialized("TCP Ethernet link has no interface");
        if (tcp.endpoints().pools().prepared) return Status::busy("TCP external packet ticket must complete first");
        if (auto drained = tcp.drain_closes(); !drained) return drained;
        if (auto timed = tcp.endpoints().tick(now); !timed) return timed;
        if (auto timed = nic->tick(now); !timed) return retire(tcp, waits, timed);
        backlog_ = false;
        if (pass.device)
        {
            auto reaped = transport.reap(budget); if (!reaped) return retire(tcp, waits, reaped.status());
            if (reaped.value().completed > budget || reaped.value().credit > maximum_credit)
                return retire(tcp, waits, Status::io_error("Ethernet TX ledger exceeds its contract"));
            credit_ = reaped.value().credit;
        }
        const usize receive_budget = pass.device ? budget : 0;
        usize frames = 0;
        for (; frames < receive_budget; ++frames)
        {
            auto received = transport.receive(incoming_);
            if (!received)
            {
                if (received.status().code() == StatusCode::not_found || received.status().code() == StatusCode::would_block) break;
                return retire(tcp, waits, received.status());
            }
            if (!received.value() || received.value() > incoming_.size())
                return retire(tcp, waits, Status::io_error("Ethernet copied RX length is invalid"));
            add(received_);
            const auto packet = nic->receive(wire::Bytes{incoming_.data(), received.value()}, now);
            if (packet.family == Delivery::control) continue;
            if (packet.family != Delivery::ipv4 && packet.family != Delivery::ipv6) { add(dropped_); continue; }
            if (packet.protocol == wire::protocol_tcp)
            {
                auto accepted = tcp.endpoints().ingest(packet.packet, now, iss);
                if (!accepted || accepted.value().dropped) add(dropped_);
                else add(tcp_received_);
                continue;
            }
            if (packet.protocol != wire::protocol_udp) { add(dropped_); continue; }
            auto accepted = udp.deliver(packet.packet);
            if (!accepted)
            {
                if (!udp.status()) return retire(tcp, waits, udp.status());
                add(dropped_); continue;
            }
            if (accepted.value().kind == NativeUdpFrameKind::udp_queued) add(udp_received_);
            else
            {
                if (accepted.value().kind == NativeUdpFrameKind::unbound) nic->unreachable(packet);
                add(dropped_);
            }
        }
        backlog_ = pass.device && frames == budget;
        if (auto drained = tcp.drain_closes(); !drained) return retire(tcp, waits, drained);
        // Interface frames (replies, solicitations) first, then packets whose
        // neighbour resolved, then TCP segments.
        Status failure{};
        for (usize i = 0; i < budget && credit_ && nic->control(); ++i)
        {
            auto copied = transport.try_transmit(*nic->control());
            if (!copied)
            {
                if (copied.code() == StatusCode::would_block) { credit_ = 0; break; }
                return retire(tcp, waits, copied);
            }
            nic->pop_control(); --credit_; add(submitted_);
        }
        if (credit_)
            nic->flush([&](wire::Bytes frame) {
                if (!credit_ || !failure.ok()) return false;
                auto copied = transport.try_transmit(frame);
                if (!copied)
                {
                    if (copied.code() == StatusCode::would_block) credit_ = 0; else failure = copied;
                    return false;
                }
                --credit_; add(submitted_); return true;
            });
        if (!failure) return retire(tcp, waits, failure);
        for (usize i = 0; i < budget && credit_; ++i)
        {
            auto prepared = tcp.endpoints().prepare(std::span{outgoing_}.subspan(14), now);
            if (!prepared) return retire(tcp, waits, prepared.status());
            if (!prepared.value()) break;
            const auto packet = *prepared.value();
            // A held or unroutable segment counts as sent: retransmission
            // recovers it, as it would a segment lost on the wire.
            auto framed = nic->output(outgoing_, packet.length);
            if (!framed || !framed.value())
            {
                if (auto committed = tcp.endpoints().complete(packet.ticket, true, now); !committed) return retire(tcp, waits, committed);
                continue;
            }
            auto copied = transport.try_transmit({outgoing_.data(), framed.value()});
            auto committed = tcp.endpoints().complete(packet.ticket, bool(copied), now);
            if (!committed) return retire(tcp, waits, committed);
            if (!copied)
            {
                if (copied.code() == StatusCode::would_block) { credit_ = 0; break; }
                return retire(tcp, waits, copied);
            }
            --credit_; add(submitted_);
        }
        controls_ = nic->control_pending(); dropped_control_ = nic->stats().control_dropped;
        return waits.poll(pass.device ? pass.source : sched::WakeSource::thread);
    }
  private:
    static void add(u64 &count) { if (count != ~u64{0}) ++count; }
    std::array<std::byte, 1514> incoming_{}, outgoing_{};
    usize controls_{}, credit_{};
    u64 received_{}, tcp_received_{}, udp_received_{}, submitted_{}, dropped_{}, dropped_control_{};
    bool retired_{}, backlog_{}; StatusCode cause_{StatusCode::ok};
};
}
