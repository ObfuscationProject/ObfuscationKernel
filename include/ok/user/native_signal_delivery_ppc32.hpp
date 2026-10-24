#pragma once

#include "ok/user/native_signal_context_ppc32.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_trampoline.hpp"

namespace ok::user::native
{

struct Ppc32SignalFramePolicy
{
    using Frame = arch::execution::UserFrame;
    using Trampoline = SignalTrampoline<abi::linux_user::Profile::ppc>;

    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <Ppc32SignalFormat Format, typename Write>
        requires requires(Write &write, u64 address, std::span<const std::byte> bytes) {
            { write(address, bytes) } -> std::same_as<bool>;
        }
    static bool prepare_kind(const Frame &frame, sched::SignalMask blocked, sched::PendingSignalInfo info,
                             const sched::SignalAction &action, const sched::SignalAltstack &stack,
                             u64 begin, u64 end, const Ppc32SignalContext &context, Write &&write,
                             Frame &out, u64 mapped_trampoline)
    {
        using C = Ppc32SignalFrame<Format>;
        const auto captured = context.template codec<Format>();
        if (!captured.metadata_supplied || !C::supported(frame, captured.contract)) return false;
        const bool entering = (action.flags & 0x08000000) && stack.status(frame.sp) == 0;
        const bool nested = stack.on_stack(frame.sp);
        u64 top = frame.sp;
        if (entering)
        {
            if (stack.base >= end || stack.size > end - stack.base) return false;
            top = stack.base + stack.size;
        }
        typename C::Placement placement;
        if (!C::place(top, begin, end, placement)) return false;
        // Contain the lower handler frame, saved context and untouched upper
        // ABI gap. Linux get_sigframe chooses sigsp, then rounds down to16.
        if ((entering || nested) &&
            (placement.stack_pointer < stack.base || stack.base >= end || stack.size > end - stack.base ||
             placement.stack_pointer > stack.base + stack.size ||
             C::allocation_bytes > stack.base + stack.size - placement.stack_pointer)) return false;
        u64 restorer{};
        constexpr auto entry = C::realtime ? SignalTrampolineEntry::realtime : SignalTrampolineEntry::legacy;
        if (!Trampoline::entry_address(mapped_trampoline, entry, begin, end, restorer)) return false;
        const typename C::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        typename C::Image image;
        Frame staged;
        // Native PPC ignores the action's SA_RESTORER address. Only the owned,
        // fully protected dual-entry mapping supplied by the caller is used.
        if (!C::encode(frame, captured, blocked, {stack.base, stack.size, stack.flags}, sender,
                       placement, action.handler, begin, end, image) ||
            !C::setup_handler(frame, captured.contract, placement, sender, action.handler, restorer,
                              begin, end, staged) ||
            !write(placement.stack_pointer, std::span<const std::byte>{image.bytes})) return false;
        out = staged;
        return true;
    }

    template <typename Write>
    static bool prepare(const Frame &frame, sched::SignalMask blocked, sched::PendingSignalInfo info,
                        const sched::SignalAction &action, const sched::SignalAltstack &stack,
                        u64 begin, u64 end, const Ppc32SignalContext &context, Write &&write,
                        Frame &out, u64 mapped_trampoline)
    {
        if (action.flags & 4)
            return prepare_kind<Ppc32SignalFormat::realtime>(frame, blocked, info, action, stack,
                begin, end, context, write, out, mapped_trampoline);
        return prepare_kind<Ppc32SignalFormat::legacy>(frame, blocked, info, action, stack,
            begin, end, context, write, out, mapped_trampoline);
    }
};

using Ppc32SignalDelivery = SignalDelivery<abi::linux_user::Profile::ppc, Ppc32SignalFramePolicy>;

} // namespace ok::user::native
