#pragma once
#include "ok/user/native_signal_readiness_mips_o32.hpp"
#include "ok/user/native_signal_delivery_mips_o32.hpp"

namespace ok::user::native
{
enum class MipsO32HandlerGate : u8 { proceed, defer_handler, unverified };
// Inspect only. Runtime calls the immutable delivery policy on proceed, enters
// the unchanged interrupted frame on defer_handler, and rejects a missing
// trusted lifecycle hook on unverified. No ticket or signal state is modified.
template <typename Queue>
[[nodiscard]] MipsO32HandlerGate mips_o32_handler_gate(
    const Queue &pending, const sched::SignalActionTable &actions, sched::SignalMask blocked,
    const MipsO32SignalReadiness &readiness, MipsO32SignalReadiness::Identity identity,
    MipsO32SignalReadiness::Capability capability) noexcept
{
    typename Queue::Delivery ticket;
    if (!pending.peek(blocked, ticket)) return MipsO32HandlerGate::proceed;
    const auto *action = actions.find(ticket.info.signal);
    using Delivery = MipsO32SignalDelivery;
    if (!action || Delivery::Actions::ignored(ticket.info.signal, *action) ||
        action->handler == sched::SignalAction::default_handler || !Delivery::supported(*action))
        return MipsO32HandlerGate::proceed;
    if (!capability.admitted() || readiness.identity() != identity) return MipsO32HandlerGate::unverified;
    if (readiness.ready(identity)) return MipsO32HandlerGate::proceed;
    return readiness.state() == MipsO32SignalReadiness::State::timer_bd
        ? MipsO32HandlerGate::defer_handler : MipsO32HandlerGate::unverified;
}
} // namespace ok::user::native
