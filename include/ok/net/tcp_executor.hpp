#pragma once
#include <ok/net/tcp_endpoints.hpp>

namespace ok::net::tcp
{
struct ExecutorTransmitState { usize completed{}, credit{}; };
template <typename Transport>
concept CopiedIpv4Transport = requires(Transport &transport, usize budget,
    std::span<std::byte> destination, Bytes packet)
{
    { transport.reap(budget) } -> std::same_as<Result<ExecutorTransmitState>>;
    { transport.receive(destination) } -> std::same_as<Result<usize>>;
    { transport.try_transmit(packet) } -> std::same_as<Status>;
};
struct ExecutorState
{
    bool retired{}; usize transmit_credit{}; u64 received{}, submitted{}, dropped{};
    StatusCode cause{StatusCode::ok};
};

// This adapter requires complete copied IPv4 RX and full-copy-on-success TX;
// it is not an Ethernet, route, DMA or clock provider. Exactly one trusted clock
// sample is validated before any driver mutation, then reused for all commits.
template <typename Table, usize ReceiveBudget = 8, usize TransmitBudget = 8, usize MaximumCredit = 8>
class Executor final
{
    static_assert(ReceiveBudget > 0 && TransmitBudget > 0 && MaximumCredit > 0);
  public:
    Executor() = default;
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    Executor(Executor &&) = delete;
    Executor &operator=(Executor &&) = delete;
    [[nodiscard]] ExecutorState state() const { return {retired_, credit_, received_, submitted_, dropped_, cause_}; }
    Status retire(Table &table, Status reason)
    {
        auto drained = table.retire(); if (!drained) return drained;
        retired_ = true; credit_ = 0; cause_ = reason.code();
        return Status::io_error("TCP executor transport is retired");
    }
    template <CopiedIpv4Transport Transport, IssSource Source>
    Status poll(Table &table, Transport &transport, Source &source, u64 now)
    {
        if (retired_) return Status::io_error("TCP executor transport is retired");
        if (table.pools().prepared) return Status::busy("complete external TCP table ticket before executor polling");
        auto timed = table.tick(now); if (!timed) return timed;
        auto completion = transport.reap(TransmitBudget);
        if (!completion) return retire(table, completion.status());
        if (completion.value().completed > TransmitBudget || completion.value().credit > MaximumCredit)
            return retire(table, Status::io_error("TCP transport credit/completion exceeds its bounded contract"));
        credit_ = completion.value().credit;
        for (usize i = 0; i < ReceiveBudget; ++i)
        {
            auto packet = transport.receive(incoming_);
            if (!packet)
            {
                if (packet.status().code() == StatusCode::would_block || packet.status().code() == StatusCode::not_found) break;
                return retire(table, packet.status());
            }
            if (!packet.value() || packet.value() > incoming_.size())
                return retire(table, Status::io_error("TCP transport returned invalid IPv4 copy length"));
            add(received_);
            auto ingested = table.ingest({incoming_.data(), packet.value()}, now, source);
            if (!ingested || ingested.value().dropped) add(dropped_);
        }
        for (usize i = 0; i < TransmitBudget && credit_; ++i)
        {
            auto packet = table.prepare(outgoing_, now);
            if (!packet)
            {
                if (packet.status().code() == StatusCode::overflow) return retire(table, packet.status());
                return packet.status();
            }
            if (!packet.value()) break;
            const auto selected = *packet.value();
            auto submitted = transport.try_transmit({outgoing_.data(), selected.length});
            // No second clock sample or callback into the table is permitted
            // between driver copy and commit. Device completion remains separate.
            auto completed = table.complete(selected.ticket, bool(submitted), now);
            if (!completed) return retire(table, completed);
            if (!submitted)
            {
                if (submitted.code() == StatusCode::would_block) { credit_ = 0; break; }
                return retire(table, submitted);
            }
            --credit_; add(submitted_);
        }
        return Status::success();
    }
  private:
    static void add(u64 &counter) { if (counter != ~u64{0}) ++counter; }
    std::array<std::byte, 1500> incoming_{};
    std::array<std::byte, Table::packet_capacity> outgoing_{};
    usize credit_{}; u64 received_{}, submitted_{}, dropped_{};
    bool retired_{}; StatusCode cause_{StatusCode::ok};
};
}
