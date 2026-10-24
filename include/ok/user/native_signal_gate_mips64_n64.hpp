#pragma once
#include "ok/user/native_signal_readiness_mips64_n64.hpp"
#include "ok/user/native_signal_delivery_mips64_n64.hpp"
namespace ok::user::native
{
enum class Mips64N64HandlerGate : u8 { proceed, defer_handler, unverified };
template<typename Queue>
[[nodiscard]] Mips64N64HandlerGate mips64_n64_handler_gate(
    const Queue &pending, const sched::SignalActionTable &actions, sched::SignalMask blocked,
    const Mips64N64SignalReadiness &readiness, Mips64N64SignalReadiness::Identity identity,
    const arch::execution::UserFrame &frame, Mips64N64SignalReadiness::Capability capability) noexcept
{
    typename Queue::Delivery ticket;
    if(!pending.peek(blocked,ticket)) return Mips64N64HandlerGate::proceed;
    const auto *action=actions.find(ticket.info.signal);
    using Delivery=Mips64N64SignalDelivery;
    if(!action || Delivery::Actions::ignored(ticket.info.signal,*action) ||
        action->handler==sched::SignalAction::default_handler || !Delivery::supported(*action))
        return Mips64N64HandlerGate::proceed;
    if(!capability.admitted() || !(readiness.identity()==identity)) return Mips64N64HandlerGate::unverified;
    if(readiness.ready(identity,frame,capability)) return Mips64N64HandlerGate::proceed;
    return readiness.deferred(identity,frame,capability)
        ? Mips64N64HandlerGate::defer_handler : Mips64N64HandlerGate::unverified;
}
} // namespace ok::user::native
