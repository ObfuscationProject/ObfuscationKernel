#pragma once

#include "ok/user/native_signal_frame_loongarch64.hpp"
#include "ok/user/native_signal_altstack.hpp"
#include "ok/user/native_signal_force.hpp"

namespace ok::user::native
{

struct Loongarch64SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Codec = Loongarch64SignalFrame;

    // Caller serializes all task state and user copies. After the last fallible
    // operation, publish the queue, disposition, frame, mask and alternate stack
    // before any next user entry. Callbacks cannot mutate these owners.
    template <typename Queue, typename Read, typename Write, typename CanEnter, typename SizeValid>
    static bool restore(Queue &pending, sched::SignalActionTable &actions,
                        sched::SignalMask &blocked, Frame &frame, sched::SignalAltstack &stack,
                        u64 begin, u64 end, Read &&read, Write &&write,
                        CanEnter &&can_enter, SizeValid &&size_valid)
    {
        Codec::Restored staged;
        if (!Codec::decode(frame.sp, frame, begin, end, read, staged) || !can_enter(staged.frame)) return false;
        auto next_stack = stack;
        apply_restored_altstack<abi::linux_user::Profile::loongarch64>(next_stack,
            {staged.stack.pointer, staged.stack.size, staged.stack.flags}, staged.frame.sp, size_valid);
        if (staged.fcsr_patch.required)
        {
            std::array<std::byte, 4> bytes{};
            Codec::put(bytes, 0, staged.fcsr_patch.value, bytes.size());
            if (!write(staged.fcsr_patch.address, std::span<const std::byte>{bytes})) return false;
        }
        if (staged.pending_fp_exception && !force_kernel_signal(pending, actions, staged.mask, 8)) return false;
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        return true;
    }
};

} // namespace ok::user::native
