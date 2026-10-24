#pragma once

#include "ok/user/native_signal_frame_aarch64.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

struct Aarch64SignalFramePolicy
{
    using Frame = Aarch64SignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        // This initial private profile requires an explicit userspace restorer.
        // The fallback Kernel trampoline is a separate mapped-code contract.
        return action.handler <= 1 || (action.flags & 0x04000000);
    }
    template <typename Write>
    static bool prepare(const Frame::Frame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack,
                        u64 begin, u64 end, u32, Write &&write, Frame::Frame &out, u64)
    {
        Frame::Placement placement;
        const bool nested = altstack.on_stack(frame.sp);
        const bool entering = (action.flags & 0x08000000) && altstack.status(frame.sp) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        if (!Frame::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        Frame::Image image;
        Frame::Frame next;
        auto raw_stack = [](Frame::Stack raw, Frame::Stack &normalized) noexcept {
            normalized = raw; return true;
        };
        if (!Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags},
                           sender, begin, end, image, raw_stack) ||
            !Frame::setup_handler(frame, placement, sender, action.handler, action.restorer,
                                  (action.flags & 4) != 0, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};
using Aarch64SignalDelivery = SignalDelivery<abi::linux_user::Profile::aarch64, Aarch64SignalFramePolicy>;

} // namespace ok::user::native
