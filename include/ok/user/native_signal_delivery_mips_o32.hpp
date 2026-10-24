#pragma once

#include "ok/user/native_signal_context_mips_o32.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_trampoline.hpp"

namespace ok::user::native {

struct MipsO32SignalFramePolicy {
  using Frame = MipsO32RtSignalFrame;
  using Trampoline = SignalTrampoline<abi::linux_user::Profile::mips>;
  [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) {
    return true;
  }

  template <MipsO32SignalFormat Format, typename Write>
  static bool
  prepare_kind(const arch::execution::UserFrame &frame,
               sched::SignalMask blocked, sched::PendingSignalInfo info,
               const sched::SignalAction &action,
               const sched::SignalAltstack &stack, u64 begin, u64 end,
               const MipsO32SignalContext &context, Write &&write,
               arch::execution::UserFrame &out, u64 mapped_trampoline) {
    using C = MipsO32SignalFrame<Format>;
    if (frame.sp > C::word_max)
      return false;
    const auto captured = context.template codec<Format>();
    if (!C::capture(frame, captured))
      return false;
    const bool nested = stack.on_stack(frame.sp);
    // Match Linux's overflow check before its emulator gap and sigsp().
    if (nested && !stack.on_stack(static_cast<u32>(frame.sp - C::frame_bytes)))
      return false;
    const u32 after_gap = static_cast<u32>(frame.sp - 32);
    const bool entering =
        (action.flags & 0x08000000) && stack.status(after_gap) == 0;
    u64 top = frame.sp;
    if (entering) {
      if (stack.base >= end || stack.size > end - stack.base)
        return false;
      top = stack.base + stack.size;
    }
    typename C::Placement placement;
    if (!C::place(top, entering, begin, end, placement))
      return false;
    // Also contain alignment/gap bytes; this is stricter than Linux's
    // pre-gap overflow check, especially at alternate-stack boundaries.
    if ((nested || entering || stack.on_stack(after_gap)) &&
        !stack.physically_contains(placement.address))
      return false;
    u64 restorer{};
    constexpr auto entry = C::realtime ? SignalTrampolineEntry::realtime
                                       : SignalTrampolineEntry::legacy;
    if (!Trampoline::entry_address(mapped_trampoline, entry, begin, end,
                                   restorer))
      return false;
    const typename C::Info sender{static_cast<i32>(info.signal), info.code,
                                  info.sender_pid, info.sender_uid, info.child_status};
    typename C::Image image;
    arch::execution::UserFrame next;
    if (!C::encode(frame, captured, blocked,
                   {stack.base, stack.size, stack.flags}, sender, begin, end,
                   image) ||
        !C::setup_handler(frame, captured, placement, sender, action.handler,
                          restorer, begin, end, next))
      return false;
    if (!write(placement.address, std::span<const std::byte>{image.bytes}))
      return false;
    out = next;
    return true;
  }

  template <typename Write>
  static bool prepare(const arch::execution::UserFrame &frame,
                      sched::SignalMask blocked, sched::PendingSignalInfo info,
                      const sched::SignalAction &action,
                      const sched::SignalAltstack &stack, u64 begin, u64 end,
                      const MipsO32SignalContext &context, Write &&write,
                      arch::execution::UserFrame &out, u64 mapped_trampoline) {
    if (action.flags & 8)
      return prepare_kind<MipsO32SignalFormat::realtime>(
          frame, blocked, info, action, stack, begin, end, context, write, out,
          mapped_trampoline);
    return prepare_kind<MipsO32SignalFormat::legacy>(
        frame, blocked, info, action, stack, begin, end, context, write, out,
        mapped_trampoline);
  }
};

using MipsO32SignalDelivery =
    SignalDelivery<abi::linux_user::Profile::mips, MipsO32SignalFramePolicy>;

} // namespace ok::user::native
