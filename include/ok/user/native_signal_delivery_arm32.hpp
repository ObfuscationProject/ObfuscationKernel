#pragma once

#include "ok/arch/arm32/fp_state.hpp"
#include "ok/user/native_signal_frame_arm32.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

struct Arm32SignalFramePolicy
{
    using Frame = Arm32RtSignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        return action.handler <= 1 || (action.flags & 0x04000000);
    }

    template <Arm32SignalFormat Format, typename Write>
    static bool prepare_kind(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                             sched::PendingSignalInfo info, const sched::SignalAction &action,
                             const sched::SignalAltstack &altstack, u64 begin, u64 end,
                             Write &&write, arch::execution::UserFrame &out)
    {
        using Codec = Arm32SignalFrame<Format>;
        using Fp = arch::execution::Arm32FpState;
        if (!(action.flags & 0x04000000) || !Fp::admitted(frame)) return false;
        const bool nested = altstack.on_stack(frame.sp);
        const bool entering = (action.flags & 0x08000000) && altstack.status(frame.sp) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        typename Codec::Placement placement;
        if (!Codec::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        // FPEXC comes from the actual trap capture. This synchronous D32 profile
        // excludes pending exception/FPINST state; it does not claim to capture it.
        const typename Codec::ExceptionState exception{Fp::fpexc(frame), 0, 0};
        const typename Codec::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        typename Codec::Image image;
        arch::execution::UserFrame next;
        if (!Codec::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender,
                           exception, action.handler & 1, true, begin, end, image) ||
            !Codec::setup_handler(frame, placement, sender, action.handler, {action.restorer, true},
                                  begin, end, next) ||
            !write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack, u64 begin, u64 end, u32,
                        Write &&write, arch::execution::UserFrame &out, u64)
    {
        if (action.flags & 4)
            return prepare_kind<Arm32SignalFormat::realtime>(frame, blocked, info, action, altstack,
                                                             begin, end, write, out);
        return prepare_kind<Arm32SignalFormat::legacy>(frame, blocked, info, action, altstack,
                                                      begin, end, write, out);
    }
};

using Arm32SignalDelivery = SignalDelivery<abi::linux_user::Profile::arm32, Arm32SignalFramePolicy>;

} // namespace ok::user::native
