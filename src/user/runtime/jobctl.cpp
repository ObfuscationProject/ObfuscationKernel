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
constexpr std::array<u32, 4> jobctl_stop_signals{JobActions::stop_signal, JobActions::tstp_signal,
                                                 JobActions::ttin_signal, JobActions::ttou_signal};

// A mask that admits only `signal`.
sched::SignalMask jobctl_only(u32 signal)
{
    sched::SignalMask mask;
    for (auto &word : mask.words) word = ~u64{0};
    mask.set(signal, false);
    return mask;
}

// Consume a pending default stop that `thread` would take, from its directed
// queue first, then the shared queue. `shared` is the shared pending mask.
// Returns the signal, or 0.
u32 jobctl_take_stop(PrivateSignalActions &state, const sched::SignalMask &shared, sched::ThreadControlBlock &thread)
{
    const auto *wait = signal_waits.real_blocked(thread) ? signal_waits.find(thread) : nullptr;
    auto *directed = private_signal_queue(thread);
    const auto mine = directed ? directed->pending_mask() : sched::SignalMask{};
    for (const auto signal : jobctl_stop_signals)
    {
        if (!shared.contains(signal) && !mine.contains(signal)) continue;
        const auto *action = state.table.find(signal);
        if (!action || action->handler != sched::SignalAction::default_handler ||
            thread.native_signal_mask.contains(signal)) continue;
        // sigtimedwait dequeues a signal it waits for instead of stopping.
        if (wait && wait->request.wanted.contains(signal)) continue;
        const auto only = jobctl_only(signal);
        if (directed)
        {
            PrivateSignalQueue::Delivery ticket;
            if (directed->peek(only, ticket)) return directed->consume(ticket) ? signal : 0;
        }
        PrivateProcessQueue::Delivery ticket;
        if (state.shared.peek(only, ticket)) return state.shared.consume(ticket) ? signal : 0;
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
        const auto shared = state->shared.pending_mask();
        for (auto &thread : process.threads())
        {
            if (!thread.native_active || thread.state != sched::ProcessState::blocked) continue;
            const auto signal = jobctl_take_stop(*state, shared, thread);
            if (!signal || private_jobctl_discard_stop(kernel, process, signal)) continue;
            private_jobctl_stop(kernel, process, signal);
            break;
        }
    }
}
#endif
} // namespace runtime_detail
} // namespace ok::user::native
