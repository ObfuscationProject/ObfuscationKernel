#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_action.hpp"
#include "ok/sched/pending_signals.hpp"
#include "ok/user/native_signal_altstack.hpp"

namespace ok::user::native
{

// Common serialized transaction; FramePolicy owns architecture frame layout and
// register setup. A failed user copy can change user bytes, never live context.
template <abi::linux_user::Profile Profile, typename FramePolicy>
struct SignalDelivery
{
    using Frame = typename FramePolicy::Frame;
    using Actions = SignalActionCodec<Profile>;
    enum class Result : u8 { none, ignored, handler, terminate, unsupported, corrupt };
    static constexpr u64 siginfo = Actions::mips ? 8 : 4;
    static constexpr u64 restorer = Actions::has_restorer ? 0x04000000 : 0;
    static constexpr u64 nodefer = 0x40000000, resethand = 0x80000000;
    static constexpr u64 onstack = 0x08000000;
    // Private runtime admission only: caught sleep returns EINTR even with this
    // flag. Restartable pipe/futex/child waits retain separate blocked gates.
    static constexpr u64 restart = 0x10000000;
    // Child-stop policy is retained as a no-op in this exit-only private slice.
    static constexpr u64 supported_flags = siginfo | restorer | nodefer | resethand | onstack | restart |
        Actions::no_child_stop | Actions::no_child_wait;

    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        return !(action.flags & ~supported_flags) && FramePolicy::supported(action);
    }

    template <typename Queue, typename Context, typename Write, typename CanEnter>
    static Result deliver(Queue &pending, sched::SignalActionTable &actions,
                          sched::SignalMask &blocked, arch::execution::UserFrame &frame,
                          sched::SignalAltstack &altstack,
                          u64 begin, u64 end, const Context &cpu_context, Write &&write,
                          u32 &selected_signal, u64 mapped_restorer, CanEnter &&can_enter,
                          const sched::SignalMask *saved_mask = nullptr)
    {
        typename Queue::Delivery ticket;
        if (!pending.peek(blocked, ticket)) return Result::none;
        const auto signal = ticket.info.signal;
        selected_signal = signal;
        auto *entry = actions.find(signal);
        if (!entry) return Result::corrupt;
        const auto action = *entry;
        if (Actions::ignored(signal, action))
            return pending.consume(ticket) ? Result::ignored : Result::corrupt;
        if (action.handler == sched::SignalAction::default_handler)
        {
            // Job control remains an explicit runtime integration gate.
            constexpr u32 first_job_stop = Actions::mips ? 23 : 19;
            if (signal == first_job_stop || signal == (Actions::mips ? 24 : 20) ||
                signal == (Actions::mips ? 26 : 21) || signal == (Actions::mips ? 27 : 22))
                return Result::unsupported;
            return pending.consume(ticket) ? Result::terminate : Result::corrupt;
        }
        if (!supported(action)) return Result::unsupported;
        arch::execution::UserFrame staged;
        if (!FramePolicy::prepare(frame, saved_mask ? *saved_mask : blocked, ticket.info, action, altstack, begin, end,
                                  cpu_context, write, staged, mapped_restorer) || !can_enter(staged))
        {
            selected_signal = 11;
            return Result::terminate;
        }
        if (!pending.consume(ticket)) return Result::corrupt;
        auto mask = blocked;
        for (usize i = 0; i < mask.words.size(); ++i) mask.words[i] |= action.mask.words[i];
        if (!(action.flags & nodefer)) mask.set(signal);
        blocked = Actions::Mask::sanitize(mask);
        if (action.flags & resethand) entry->handler = sched::SignalAction::default_handler;
        frame = staged;
        altstack.disarm_after_frame_commit();
        return Result::handler;
    }

    // Byte/transaction models have no address space. Runtime callers must use
    // the checked overload to admit the staged frame before consuming a signal.
    template <typename Queue, typename Context, typename Write>
    static Result deliver(Queue &pending, sched::SignalActionTable &actions,
                          sched::SignalMask &blocked, arch::execution::UserFrame &frame,
                          sched::SignalAltstack &altstack,
                          u64 begin, u64 end, const Context &cpu_context, Write &&write,
                          u32 &selected_signal, u64 mapped_restorer = 0)
    {
        return deliver(pending, actions, blocked, frame, altstack, begin, end,
                       cpu_context, write, selected_signal, mapped_restorer,
                       [](const arch::execution::UserFrame &) { return true; });
    }
};

} // namespace ok::user::native
