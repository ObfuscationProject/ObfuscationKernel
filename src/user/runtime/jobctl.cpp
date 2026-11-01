// Job control in the runtime: applying a default stop that delivery consumed,
// and the settle pass that stops a Linux process whose pending stop signal is
// visible only to threads parked in waits (a stop never interrupts a wait).
// The scheduler overlay keeps a stopped process from being entered; its waits
// stay armed and still complete.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
#ifdef OK_PRIVATE_POLL_MASK
namespace
{
using JobActions = SignalActionCodec<private_signal_profile>;
using JobPendingView = SignalPendingView<PrivateSignalQueue, PrivateProcessQueue>;

// Consume a pending default stop only when it is the signal `thread` would
// dequeue next in Linux order (directed queue, then shared, lowest number
// first). Ignored entries are passed over as get_signal discards them; a lower
// fatal or caught signal comes first and the wait selectors deliver it.
// Returns the signal, or 0.
u32 jobctl_take_stop(PrivateSignalActions &state, sched::ThreadControlBlock &thread)
{
    JobPendingView view{private_signal_queue(thread), &state.shared};
    if (!view) return 0;
    auto blocked = thread.native_signal_mask;
    // sigtimedwait dequeues a signal it waits for instead of stopping.
    if (signal_waits.real_blocked(thread))
        if (const auto *wait = signal_waits.find(thread))
            for (usize i = 0; i < blocked.words.size(); ++i) blocked.words[i] |= wait->request.wanted.words[i];
    JobPendingView::Delivery next;
    while (view.peek(blocked, next))
    {
        const auto signal = next.info.signal;
        const auto *action = state.table.find(signal);
        if (!action) return 0;
        if (JobActions::ignored(signal, *action)) { blocked.set(signal); continue; }
        if (action->handler != sched::SignalAction::default_handler || !JobActions::default_stop(signal)) return 0;
        return view.consume(next) ? signal : 0;
    }
    return 0;
}
} // namespace

bool private_jobctl_discard_stop(Kernel &kernel, const sched::ProcessControlBlock &process, u32 signal)
{
    // Linux get_signal: TSTP, TTIN and TTOU to an orphaned group are dropped.
    return signal != JobActions::stop_signal &&
        kernel.scheduler().process_group_orphaned(process.process_group_id());
}

void private_jobctl_stop(Kernel &kernel, sched::ProcessControlBlock &process, u32 signal)
{
    // The parent's CLD_STOPPED notification arrives with the generation core.
    if (auto status = kernel.scheduler().stop_process(process.pid(), static_cast<u8>(signal)); !status)
    {
        runtime_failure = status;
        started = false;
    }
}

void private_jobctl_settle(Kernel &kernel)
{
    for (auto &process : kernel.scheduler().processes())
    {
        if (process.resources().personality != Personality::linux || process.job_control().stopped ||
            process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited)
            continue;
        auto *state = private_signal_find(process.pid());
        if (!state) continue;
        for (auto &thread : process.threads())
        {
            if (!thread.native_active || thread.state != sched::ProcessState::blocked) continue;
            const auto signal = jobctl_take_stop(*state, thread);
            if (!signal || private_jobctl_discard_stop(kernel, process, signal)) continue;
            private_jobctl_stop(kernel, process, signal);
            break;
        }
    }
}
#endif
} // namespace runtime_detail
} // namespace ok::user::native
