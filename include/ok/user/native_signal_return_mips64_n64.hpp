#pragma once

#include "ok/user/native_signal_frame_mips64_n64.hpp"
#include "ok/user/native_signal_altstack.hpp"
#include "ok/user/native_signal_force.hpp"

namespace ok::user::native
{

struct Mips64N64SignalReturn
{
    using Codec = Mips64N64SignalFrame;
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;

    template <typename Queue, typename Read, typename Write, typename CanEnter, typename SizeValid>
    static bool restore(Operation operation, Queue &pending, sched::SignalActionTable &actions,
                        sched::SignalMask &blocked, Frame &frame, sched::SignalAltstack &stack,
                        const Codec::Capture &context, u64 begin, u64 end,
                        Read &&read, Write &&write, CanEnter &&can_enter, SizeValid &&size_valid)
    {
        // Native n64 has only RT return, regardless of the action's SIGINFO flag.
        if (operation != Operation::rt_sigreturn) return false;
        Codec::ReturnPlan staged;
        if (!Codec::decode(frame.sp, frame, context, begin, end, read, staged) ||
            !can_enter(staged.frame)) return false;
        auto next_stack = stack;
        apply_restored_altstack<abi::linux_user::Profile::mips64>(next_stack,
            {staged.stack.pointer, staged.stack.size, staged.stack.flags}, staged.frame.sp, size_valid);
        if (staged.fcsr_patch.required &&
            !write(staged.fcsr_patch.address, std::span<const std::byte>{staged.fcsr_patch.bytes})) return false;
        if (staged.pending == Codec::Pending::sigfpe && !force_kernel_signal(pending, actions, staged.mask, 8))
            return false;
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        // Runtime suppresses ordinary completion to preserve restored v0/a3.
        return true;
    }
};

} // namespace ok::user::native
