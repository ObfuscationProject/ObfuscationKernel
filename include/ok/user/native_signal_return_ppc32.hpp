#pragma once

#include "ok/user/native_signal_context_ppc32.hpp"
#include "ok/user/native_signal_altstack.hpp"

namespace ok::user::native
{

struct Ppc32SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;
    using Metadata = Ppc32SignalContext::Metadata;
    static constexpr bool restore_all_no_restart = true;
    static constexpr bool checked_user_patch_required = false;
    static constexpr bool forced_fp_signal_required = false;
    struct Restored
    {
        Frame frame{};
        sched::SignalMask mask{};
        sched::SignalAltstack stack{};
        // Untrusted ABI diagnostics; never privileged trap dispatch authority.
        Metadata metadata{};
        bool restore_stack{};
    };

    template <Ppc32SignalFormat Format, typename Read, typename CanEnter>
        requires requires(Read &read, CanEnter &admit, u64 address, std::span<std::byte> bytes, const Frame &frame) {
            { read(address, bytes) } -> std::same_as<bool>;
            { admit(frame) } -> std::same_as<bool>;
        }
    static bool decode_kind(const Frame &frame, const Ppc32SignalContext &context, u64 begin, u64 end,
                            Read &&read, CanEnter &&can_enter, Restored &out)
    {
        using C = Ppc32SignalFrame<Format>;
        const auto captured = context.template codec<Format>();
        typename C::ReturnPlan plan;
        if (!captured.metadata_supplied || !C::decode(frame.sp, frame, captured.contract, begin, end, read, plan) ||
            !can_enter(plan.frame)) return false;
        const auto &meta = plan.metadata;
        out = {plan.frame, plan.mask, {plan.stack.pointer, plan.stack.size, plan.stack.flags},
               {meta.original_r3, meta.trap, meta.dar, meta.dsisr, meta.result}, plan.restore_altstack};
        return true;
    }

    template <typename Read, typename CanEnter>
    static bool decode(Operation operation, const Frame &frame, const Ppc32SignalContext &context,
                       u64 begin, u64 end, Read &&read, CanEnter &&can_enter, Restored &out)
    {
        if (operation == Operation::sigreturn)
            return decode_kind<Ppc32SignalFormat::legacy>(frame, context, begin, end, read, can_enter, out);
        if (operation == Operation::rt_sigreturn)
            return decode_kind<Ppc32SignalFormat::realtime>(frame, context, begin, end, read, can_enter, out);
        return false;
    }

    // The first read uses current SP; the second follows the editable native
    // context pointer. Callbacks serialize/stabilize copies and cannot mutate
    // these owners. Runtime must suppress ordinary r3/CR0.SO completion, own
    // eager SPE capture, and terminate a rejected return instead of resuming it.
    template <typename Read, typename CanEnter, typename SizeValid>
        requires requires(SizeValid &valid, u64 size) { { valid(size) } noexcept -> std::same_as<bool>; }
    static bool restore(Operation operation, sched::SignalMask &blocked, Frame &frame,
                        sched::SignalAltstack &stack, Metadata &diagnostics,
                        const Ppc32SignalContext &context, u64 begin, u64 end, Read &&read,
                        CanEnter &&can_enter, SizeValid &&size_valid)
    {
        Restored staged;
        if (!decode(operation, frame, context, begin, end, read, can_enter, staged)) return false;
        auto next_stack = stack;
        if (staged.restore_stack)
            apply_restored_altstack<abi::linux_user::Profile::ppc>(next_stack, staged.stack, staged.frame.sp, size_valid);
        // No user patch or forced pending-SPEFSCR signal exists in this native
        // Linux SPE path. Every fallible copy/admission precedes publication.
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        diagnostics = staged.metadata;
        return true;
    }
};

} // namespace ok::user::native
