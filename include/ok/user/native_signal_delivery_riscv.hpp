#pragma once

#include "ok/user/native_signal_frame_riscv.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

template <abi::linux_user::Profile Profile>
    requires (Profile == abi::linux_user::Profile::rv32 || Profile == abi::linux_user::Profile::rv64)
struct RiscvSignalFramePolicy
{
    using Frame = RiscvSignalFrame<Profile>;
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
        typename Frame::Placement placement;
        if (!Frame::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const typename Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        typename Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender, begin, end, image) ||
            !Frame::stage_handler(frame, placement, action.handler, mapped_restorer, sender, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

template <abi::linux_user::Profile Profile>
using RiscvSignalDelivery = SignalDelivery<Profile, RiscvSignalFramePolicy<Profile>>;

} // namespace ok::user::native
