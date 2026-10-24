#pragma once

#include "ok/user/native_signal_altstack.hpp"
#include "ok/user/native_signal_context_mips_o32.hpp"
#include "ok/user/native_signal_force.hpp"

namespace ok::user::native {

struct MipsO32SignalReturn {
  using Frame = arch::execution::UserFrame;
  using Operation = abi::linux_user::Operation;

  template <MipsO32SignalFormat Format, typename Queue, typename Read,
            typename Write, typename CanEnter, typename SizeValid>
  static bool restore_kind(Queue &pending, sched::SignalActionTable &actions,
                           sched::SignalMask &blocked, Frame &frame,
                           sched::SignalAltstack &stack,
                           const MipsO32SignalContext &context, u64 begin,
                           u64 end, Read &&read, Write &&write,
                           CanEnter &&can_enter, SizeValid &&size_valid) {
    using C = MipsO32SignalFrame<Format>;
    typename C::ReturnPlan staged;
    if (!C::decode(frame.sp, frame, context.template codec<Format>(), begin,
                   end, read, staged) ||
        !can_enter(staged.frame))
      return false;
    auto next_stack = stack;
    if constexpr (C::realtime)
      apply_restored_altstack<abi::linux_user::Profile::mips>(
          next_stack,
          {staged.stack.pointer, staged.stack.size, staged.stack.flags},
          staged.frame.sp, size_valid);
    if (staged.fcsr_patch.required &&
        !write(staged.fcsr_patch.address,
               std::span<const std::byte>{staged.fcsr_patch.bytes}))
      return false;
    if (staged.pending == C::Pending::sigfpe &&
        !force_kernel_signal(pending, actions, staged.mask, 8))
      return false;
    // No fallible operation remains after queue/disposition publication.
    frame = staged.frame;
    blocked = staged.mask;
    stack = next_stack;
    return true;
  }

  // Serialized current-task transaction. Runtime must admit executable PC,
  // writable SP-1 and the captured profile contract; suppress ordinary syscall
  // completion so restored v0/a3 survive. Legacy never restores altstack.
  template <typename Queue, typename Read, typename Write, typename CanEnter,
            typename SizeValid>
  static bool
  restore(Operation operation, Queue &pending,
          sched::SignalActionTable &actions, sched::SignalMask &blocked,
          Frame &frame, sched::SignalAltstack &stack,
          const MipsO32SignalContext &context, u64 begin, u64 end, Read &&read,
          Write &&write, CanEnter &&can_enter, SizeValid &&size_valid) {
    if (operation == Operation::rt_sigreturn)
      return restore_kind<MipsO32SignalFormat::realtime>(
          pending, actions, blocked, frame, stack, context, begin, end, read,
          write, can_enter, size_valid);
    if (operation == Operation::sigreturn)
      return restore_kind<MipsO32SignalFormat::legacy>(
          pending, actions, blocked, frame, stack, context, begin, end, read,
          write, can_enter, size_valid);
    return false;
  }
};

} // namespace ok::user::native
