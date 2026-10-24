#pragma once

#include "ok/user/native_signal_frame_i386.hpp"
#include "ok/user/native_signal_altstack.hpp"

namespace ok::user::native
{

struct I386SignalEntry
{
    using Frame = arch::execution::UserFrame;
    // Return ESP stays an offset in the selected SS. Never publish its linear
    // address as ESP; use this address only for native mapping admission.
    [[nodiscard]] static constexpr bool effective_sp(const Frame &frame, u64 begin, u64 end, u64 &out)
    {
        using Codec = I386RtSignalFrame;
        const u64 ss = frame.registers[12] & 0xffff;
        if (!Codec::stack_selector(ss) || !Codec::tls_base(frame.fs_base, begin, end) ||
            !Codec::tls_base(frame.gs_base, begin, end)) return false;
        const u64 base = ss == 0x33 ? frame.fs_base : ss == 0x3b ? frame.gs_base : 0;
        if (base > Codec::word_mask || frame.sp > Codec::word_mask - base) return false;
        const u64 address = base + frame.sp;
        if (begin >= end || end > Codec::word_mask || address <= begin || address > end) return false;
        out = address;
        return true;
    }
};

struct I386SignalReturn
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

    template <I386SignalKind Kind, typename Read, typename CanEnter>
    static bool decode_kind(const Frame &current, u64 begin, u64 end, Read &&read,
                            CanEnter &&can_enter, Restored &out)
    {
        typename I386SignalFrame<Kind>::Restored decoded;
        if (!I386SignalFrame<Kind>::decode(current.sp, current, begin, end, read, decoded)) return false;
        u64 effective{};
        if (!I386SignalEntry::effective_sp(decoded.frame, begin, end, effective) ||
            !can_enter(decoded.frame, effective)) return false;
        Restored staged{decoded.frame, decoded.mask, {}, Kind == I386SignalKind::realtime};
        if constexpr (Kind == I386SignalKind::realtime)
            staged.stack = {decoded.stack.base, decoded.stack.size, decoded.stack.flags};
        out = staged;
        return true;
    }

    // The callback must check executable PC and writable effective_sp - 1 in
    // the current address space. Only successful output may be committed.
    // Apply raw alternate-stack state only when restore_stack is true, using
    // restored architectural ESP for Linux restore_altstack's membership test.
    template <typename Read, typename CanEnter>
    static bool decode(Operation operation, const Frame &current, u64 begin, u64 end,
                       Read &&read, CanEnter &&can_enter, Restored &out)
    {
        if (operation == Operation::sigreturn)
            return decode_kind<I386SignalKind::legacy>(current, begin, end, read, can_enter, out);
        if (operation == Operation::rt_sigreturn)
            return decode_kind<I386SignalKind::realtime>(current, begin, end, read, can_enter, out);
        return false;
    }
};

} // namespace ok::user::native
