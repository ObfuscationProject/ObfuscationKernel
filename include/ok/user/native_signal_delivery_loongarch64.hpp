#pragma once

#include "ok/user/native_signal_frame_loongarch64.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

struct Loongarch64SignalFramePolicy
{
    using Frame = Loongarch64SignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack, u64 begin, u64 end, u32,
                        Write &&write, arch::execution::UserFrame &out, u64 mapped_restorer)
    {
        const bool nested = altstack.on_stack(frame.sp);
        const bool entering = (action.flags & 0x08000000) && altstack.status(frame.sp) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        Frame::Placement placement;
        if (!Frame::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender, begin, end, image) ||
            !Frame::setup_handler(frame, placement, sender, action.handler, mapped_restorer,
                                  (action.flags & 4) != 0, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

using Loongarch64SignalDelivery = SignalDelivery<abi::linux_user::Profile::loongarch64, Loongarch64SignalFramePolicy>;

} // namespace ok::user::native
