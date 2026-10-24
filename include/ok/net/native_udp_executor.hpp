#pragma once

#include "ok/net/native_udp.hpp"

namespace ok::net
{

struct NativeUdpTransmitState
{
    usize completed{}, credit{};
};

template <class Transport>
concept NativeUdpTransport = requires(Transport &transport, usize budget,
                                     std::span<std::byte> input,
                                     std::span<const std::byte> output) {
    { transport.reap(budget) } -> std::same_as<Result<NativeUdpTransmitState>>;
    { transport.receive(input) } -> std::same_as<Result<usize>>;
    { transport.try_transmit(output) } -> std::same_as<Status>;
};

struct NativeUdpExecutorState
{
    bool active{}, failed{};
    usize transmit_credit{}, pending_control{};
    u64 dropped_control{};
    StatusCode failure_cause{StatusCode::ok};
    u64 readiness_sequence{};
};

struct NativeUdpReceiveReadiness
{
    NativeUdpReadiness receive{};
    NativeUdpExecutorState executor{};
    [[nodiscard]] bool may_wait(u64 observed_receive, u64 observed_executor) const
    {
        return receive.may_wait(observed_receive) && !executor.failed &&
            executor.readiness_sequence == observed_executor;
    }
};

// One serialized boot-CPU owner. Scratch and queued control frames live in
// Kernel, never in a syscall entry frame or a borrowed driver DMA buffer.
class NativeUdpExecutor final
{
  public:
    static constexpr usize receive_budget = 8;
    static constexpr usize transmit_reap_budget = 8;
    static constexpr usize transmit_capacity = 8;
    static constexpr usize control_capacity = 2;

    NativeUdpExecutor() = default;
    explicit NativeUdpExecutor(NativeUdpReadinessCapacity capacity)
        : readiness_sequence_{capacity}
    {
        if (readiness_sequence_.terminal())
            static_cast<void>(retire(Status::overflow("native UDP executor readiness is disabled")));
    }
    NativeUdpExecutor(const NativeUdpExecutor &) = delete;
    NativeUdpExecutor &operator=(const NativeUdpExecutor &) = delete;
    NativeUdpExecutor(NativeUdpExecutor &&) = delete;
    NativeUdpExecutor &operator=(NativeUdpExecutor &&) = delete;

    [[nodiscard]] bool activated() const { return active_ || !failure_.ok(); }
    [[nodiscard]] Status status() const { return failure_; }
    [[nodiscard]] NativeUdpExecutorState state() const
    {
        return {active_, !failure_.ok(), credit_, count_, dropped_control_, failure_cause_,
                readiness_sequence_.value()};
    }
    [[nodiscard]] Result<NativeUdpReceiveReadiness> readiness(const NativeUdpService &service,
                                                            u32 handle) const
    {
        auto receive = service.readiness(handle);
        if (!receive) return receive.status();
        return NativeUdpReceiveReadiness{receive.value(), state()};
    }

    // A common Ethernet pump owns RX and the actual driver ledger. Once bound,
    // this object only publishes the existing UDP readiness interface.
    Status use_shared_link()
    {
        if (shared_link_) return failure_;
        if (activated() || count_)
            return Status::busy("UDP executor cannot transfer an active RX owner");
        shared_link_ = true;
        return {};
    }
    Status observe_shared_link(usize credit, usize controls, u64 dropped, Status outcome = {})
    {
        if (!shared_link_) return Status::denied("UDP executor is not a shared link observer");
        if (!failure_) return failure_;
        const auto before = state();
        active_ = true;
        if (credit > transmit_capacity || controls > control_capacity)
            outcome = Status::io_error("shared Ethernet ledger exceeds its contract");
        credit_ = credit; count_ = controls; dropped_control_ = dropped;
        if (!outcome) static_cast<void>(retire(outcome));
        if (before.active != active_ || before.failed != !failure_.ok() ||
            before.transmit_credit != credit_ || before.failure_cause != failure_cause_)
            if (!readiness_sequence_.advance())
                return failure_.ok()
                    ? retire(Status::overflow("shared UDP readiness is exhausted")) : failure_;
        return failure_;
    }

    template <NativeUdpTransport Transport>
    Status poll(NativeUdpService &service, Transport &transport)
    {
        if (shared_link_) return Status::denied("shared UDP observer cannot consume Ethernet RX");
        if (!failure_) return failure_;
        const auto before = state();
        auto result = poll_once(service, transport);
        if (before.active != active_ || before.failed != !failure_.ok() ||
            before.transmit_credit != credit_ || before.failure_cause != failure_cause_)
            if (!readiness_sequence_.advance())
                return failure_.ok()
                    ? retire(Status::overflow("native UDP executor readiness is exhausted")) : failure_;
        return result;
    }

  private:
    template <NativeUdpTransport Transport>
    Status poll_once(NativeUdpService &service, Transport &transport)
    {
        if (auto status = service.status(); !status) return retire(status);
        if (!service.configured()) return Status::success();
        auto reaped = transport.reap(transmit_reap_budget);
        if (!reaped) return retire(reaped.status());
        if (reaped.value().completed > transmit_reap_budget ||
            reaped.value().credit > transmit_capacity)
            return retire(Status::io_error("native UDP TX snapshot exceeds its budget"));
        active_ = true;
        credit_ = reaped.value().credit;
        for (usize at = 0; at < receive_budget; ++at)
        {
            auto received = transport.receive(incoming_);
            if (!received)
            {
                if (received.status().code() == StatusCode::not_found ||
                    received.status().code() == StatusCode::would_block) break;
                return retire(received.status());
            }
            if (!received.value() || received.value() > incoming_.size())
                return retire(Status::io_error("native UDP RX size exceeds its buffer"));
            auto frame = service.ingest({incoming_.data(), received.value()}, reply_);
            if (!frame) return retire(frame.status());
            if (frame.value().kind != NativeUdpFrameKind::arp_reply) continue;
            if (!frame.value().reply_length || frame.value().reply_length > reply_.size())
                return retire(Status::io_error("native UDP control size exceeds its buffer"));
            if (count_ == control_capacity)
            {
                if (dropped_control_ != ~u64{0}) ++dropped_control_;
                continue;
            }
            auto &control = controls_[(head_ + count_) % control_capacity];
            control.length = frame.value().reply_length;
            for (usize byte = 0; byte < control.length; ++byte)
                control.bytes[byte] = reply_[byte];
            ++count_;
        }
        if (!count_ || !credit_) return Status::success();
        const auto &control = controls_[head_];
        auto submitted = transport.try_transmit({control.bytes.data(), control.length});
        if (!submitted)
        {
            if (submitted.code() == StatusCode::would_block)
            {
                credit_ = 0;
                return Status::success();
            }
            return retire(submitted);
        }
        // Success transfers a full copy; the driver retains its own DMA slot.
        head_ = (head_ + 1) % control_capacity;
        --count_;
        --credit_;
        return Status::success();
    }

    struct ControlFrame
    {
        std::array<std::byte, NativeUdpService::frame_capacity> bytes{};
        usize length{};
    };
    Status retire(Status failure)
    {
        failure_cause_ = failure.code();
        failure_ = Status::io_error("native UDP link is retired");
        active_ = false;
        credit_ = 0;
        return failure_;
    }
    std::array<std::byte, NativeUdpService::frame_capacity> incoming_{}, reply_{};
    std::array<ControlFrame, control_capacity> controls_{};
    Status failure_{};
    StatusCode failure_cause_{StatusCode::ok};
    usize head_{}, count_{}, credit_{};
    u64 dropped_control_{};
    bool active_{}, shared_link_{};
    NativeUdpReadinessSequence<> readiness_sequence_{};
};

} // namespace ok::net
