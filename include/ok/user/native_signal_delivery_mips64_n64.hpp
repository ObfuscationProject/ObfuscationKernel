#pragma once

#include "ok/user/native_signal_frame_mips64_n64.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_trampoline.hpp"

namespace ok::user::native
{

struct Mips64N64SignalFramePolicy
{
    using Frame = Mips64N64SignalFrame;
    using Trampoline = SignalTrampoline<abi::linux_user::Profile::mips64>;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &stack, u64 begin, u64 end,
                        const Frame::Capture &context, Write &&write,
                        arch::execution::UserFrame &out, u64 mapped_trampoline)
    {
        if (!Frame::capture(frame, context)) return false;
        const bool nested = stack.on_stack(frame.sp);
        if (nested && !stack.on_stack(frame.sp - Frame::frame_bytes)) return false;
        // Native n64 subtraction is 64-bit. Do not use the o32 truncation here.
        const u64 after_gap = frame.sp - 32;
        const bool entering = (action.flags & 0x08000000) && stack.status(after_gap) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (stack.base >= end || stack.size > end - stack.base) return false;
            top = stack.base + stack.size;
        }
        Frame::Placement placement;
        if (!Frame::place(top, entering, begin, end, placement)) return false;
        // Contain the aligned/gapped placement as well as Linux's pre-gap check.
        if ((nested || entering || stack.on_stack(after_gap)) && !stack.physically_contains(placement.address))
            return false;
        u64 restorer{};
        if (!Trampoline::entry_address(mapped_trampoline, SignalTrampolineEntry::realtime,
                                        begin, end, restorer)) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, context, blocked, {stack.base, stack.size, stack.flags}, sender, begin, end, image) ||
            !Frame::setup_handler(frame, context, placement, sender, action.handler, restorer, begin, end, next))
            return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

using Mips64N64SignalDelivery = SignalDelivery<abi::linux_user::Profile::mips64, Mips64N64SignalFramePolicy>;

} // namespace ok::user::native
