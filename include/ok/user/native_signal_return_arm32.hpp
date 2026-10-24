#pragma once

#include "ok/arch/arm32/fp_state.hpp"
#include "ok/user/native_signal_frame_arm32.hpp"
#include "ok/user/native_signal_altstack.hpp"

namespace ok::user::native
{

struct Arm32SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;
    struct Restored
    {
        Frame frame{};
        sched::SignalMask mask{};
        sched::SignalAltstack stack{};
        bool restore_stack{};
    };

    template <Arm32SignalFormat Format, typename Read, typename CanEnter>
    static bool decode_kind(const Frame &current, u64 begin, u64 end, Read &&read,
                            CanEnter &&can_enter, Restored &out)
    {
        using Codec = Arm32SignalFrame<Format>;
        using Fp = arch::execution::Arm32FpState;
        typename Codec::Restored decoded;
        if (!Codec::decode(current.sp, current, begin, end, read, decoded)) return false;
        // The byte codec predates this architecture-private retained slot.
        // Publish the validated, Linux-normalized returned state explicitly.
        Fp::set_fpexc(decoded.frame, decoded.exception_state.fpexc);
        if (!Fp::admitted(decoded.frame) || !can_enter(decoded.frame)) return false;
        Restored staged{decoded.frame, decoded.mask, {}, decoded.restore_altstack};
        if (decoded.restore_altstack)
            staged.stack = {decoded.stack.pointer, decoded.stack.size, decoded.stack.flags};
        out = staged;
        return true;
    }

    // Both syscalls use the current SP directly. The callback must admit the
    // staged executable PC and writable SP-1 before any live state is changed.
    // Apply raw RT altstack metadata only after success; legacy preserves it.
    template <typename Read, typename CanEnter>
    static bool decode(Operation operation, const Frame &current, u64 begin, u64 end,
                       Read &&read, CanEnter &&can_enter, Restored &out)
    {
        if (operation == Operation::sigreturn)
            return decode_kind<Arm32SignalFormat::legacy>(current, begin, end, read, can_enter, out);
        if (operation == Operation::rt_sigreturn)
            return decode_kind<Arm32SignalFormat::realtime>(current, begin, end, read, can_enter, out);
        return false;
    }
};

} // namespace ok::user::native
