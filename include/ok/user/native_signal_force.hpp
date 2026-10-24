#pragma once

#include "ok/sched/pending_signals.hpp"
#include "ok/sched/signal_action.hpp"

namespace ok::user::native
{

// Serialized current-task force_sig subset: no ptrace or init immunity. Keep
// the first standard-signal payload on coalescing, as Linux send_signal does.
// Failed admission leaves the disposition and mask unchanged.
template <typename Queue>
bool force_kernel_signal(Queue &pending, sched::SignalActionTable &actions,
                         sched::SignalMask &blocked, u32 signal)
{
    auto *action = actions.find(signal);
    if (!action) return false;
    const auto result = pending.enqueue_kernel(signal);
    if (result != Queue::Enqueue::queued && result != Queue::Enqueue::coalesced) return false;
    if (blocked.contains(signal) || action->handler == sched::SignalAction::ignored_handler)
        action->handler = sched::SignalAction::default_handler;
    blocked.set(signal, false);
    return true;
}

} // namespace ok::user::native
