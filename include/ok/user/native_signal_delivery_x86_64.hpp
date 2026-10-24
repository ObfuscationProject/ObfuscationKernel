#pragma once

#include "ok/user/native_signal_frame_x86_64.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{

struct X86SignalFramePolicy
{
    using Frame = X86SignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        return action.handler <= 1 || (action.flags & 0x04000000);
    }
    template <typename Write>
    static bool prepare(const Frame::Frame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack,
                        u64 begin, u64 end, u32 mxcsr_mask, Write &&write, Frame::Frame &out, u64)
    {
        Frame::Placement placement;
        const bool nested = altstack.on_stack(frame.sp);
        // Linux selects the destination after unsigned red-zone adjustment.
        // A valid alternate stack can let a handler repair a damaged saved SP.
        u64 top = frame.sp - 128;
        const bool entering = (action.flags & 0x08000000) && altstack.status(top) == 0;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        if (!Frame::place_below(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid,
                                 info.sender_uid, info.child_status};
        if (!Frame::valid_info(sender)) return false;
        const auto image = Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags},
            sender, placement, action.restorer, mxcsr_mask);
        if (!write(placement.fp_address, std::span<const std::byte>{image.fp}) ||
            !write(placement.address, std::span<const std::byte>{image.prefix})) return false;
        auto next = frame;
        next.pc = action.handler;
        next.sp = placement.address;
        next.registers[9] = info.signal; // RDI.
        next.registers[10] = placement.address + Frame::info_offset; // RSI.
        next.registers[11] = placement.address + 8; // RDX: ucontext.
        next.registers[14] = 0; // RAX, including the varargs vector count.
        next.flags = (next.flags & ~u64{0x100 | 0x400 | 0x10000}) | 0x202;
        const auto fp = Frame::reset_fp();
        for (usize i = 0; i < fp.size(); ++i) next.fp_state[i] = static_cast<u8>(fp[i]);
        out = next;
        return true;
    }
};
using X86SignalDelivery = SignalDelivery<abi::linux_user::Profile::x86_64, X86SignalFramePolicy>;

} // namespace ok::user::native
