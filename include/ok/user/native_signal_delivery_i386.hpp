#pragma once

#include "ok/user/native_signal_frame_i386.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

struct I386SignalFramePolicy
{
    using Frame = I386RtSignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        return action.handler <= 1 || (action.flags & 0x04000000);
    }

    template <I386SignalKind Kind, typename Write>
    static bool prepare_kind(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                             sched::PendingSignalInfo info, const sched::SignalAction &action,
                             const sched::SignalAltstack &altstack, u64 begin, u64 end,
                             Write &&write, arch::execution::UserFrame &out)
    {
        using Codec = I386SignalFrame<Kind>;
        // Linux get_sigframe uses architectural ESP, not SS.base + ESP. The
        // historical restorer-as-stack branch is excluded by SA_RESTORER.
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
        u64 restorer{};
        if (!(action.flags & 0x04000000) ||
            !Codec::select_restorer(true, action.restorer, 0, begin, end, restorer)) return false;
        const typename Codec::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        typename Codec::Image image;
        arch::execution::UserFrame next;
        if (!Codec::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender, {},
                           placement, restorer, begin, end, image) ||
            !Codec::stage_handler(frame, placement, action.handler, sender, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes.data(), image.used})) return false;
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
            return prepare_kind<I386SignalKind::realtime>(frame, blocked, info, action, altstack,
                                                          begin, end, write, out);
        return prepare_kind<I386SignalKind::legacy>(frame, blocked, info, action, altstack,
                                                   begin, end, write, out);
    }
};

using I386SignalDelivery = SignalDelivery<abi::linux_user::Profile::i386, I386SignalFramePolicy>;

} // namespace ok::user::native
