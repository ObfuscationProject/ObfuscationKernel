#pragma once
#include "ok/core/types.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace ok::net
{
// Whole Ethernet frames a datagram send could not hand to the NIC at once
// (spec 4.13 try-transmit): the send queues its frame and returns; the
// network poller offers the queue to the NIC again, oldest first. A full
// queue refuses the frame (the caller reports ENOBUFS). Owned by the kernel
// under its net_stack lock; no waiting happens here.
template <usize Frames = 4, usize Bytes = 1514> class TransmitBacklog final
{
  public:
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] bool full() const { return count_ == Frames; }
    [[nodiscard]] usize size() const { return count_; }
    [[nodiscard]] u64 queued() const { return queued_; }
    [[nodiscard]] u64 refused() const { return refused_; }
    [[nodiscard]] u64 dropped() const { return dropped_; }

    Status push(std::span<const std::byte> frame)
    {
        if (frame.empty() || frame.size() > Bytes) return Status::invalid_argument("backlog frame size");
        if (full())
        {
            ++refused_;
            return Status::no_space("native transmit queue is full");
        }
        auto &slot = frames_[(head_ + count_) % Frames];
        for (usize index = 0; index < frame.size(); ++index) slot.bytes[index] = frame[index];
        slot.size = frame.size();
        ++count_;
        ++queued_;
        return {};
    }

    // Offers queued frames to `transmit` (an immediate try) oldest first and
    // stops at the first would_block. A frame the NIC refused otherwise is
    // dropped (counted). True when the queue is empty afterwards.
    template <typename Transmit> bool flush(Transmit &&transmit)
    {
        while (count_)
        {
            const auto &front = frames_[head_];
            const auto status = transmit(std::span<const std::byte>{front.bytes.data(), front.size});
            if (!status && status.code() == StatusCode::would_block) return false;
            if (!status) ++dropped_;
            head_ = (head_ + 1) % Frames;
            --count_;
        }
        return true;
    }

  private:
    struct Frame
    {
        usize size{};
        std::array<std::byte, Bytes> bytes{};
    };
    std::array<Frame, Frames> frames_{};
    usize head_{}, count_{};
    u64 queued_{}, refused_{}, dropped_{};
};
} // namespace ok::net
