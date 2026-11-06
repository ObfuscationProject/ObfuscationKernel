// Job control in the runtime: the signal generation core shared by every
// sender, kernel-originated signals and their owed retries, native SIG_DFL/
// SIG_IGN dispositions, parent notification, the orphaned-group pass at exit,
// applying a default stop that delivery consumed, and the settle pass that
// stops a Linux process whose pending stop signal is visible only to threads
// parked in waits (a stop never interrupts a wait). The scheduler overlay keeps
// a stopped process from being entered; its waits stay armed and still complete.
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
bool jobctl_completed(const sched::ProcessControlBlock &process)
{
    return process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited;
}
const job::Numbering &jobctl_numbering(const sched::ProcessControlBlock &process)
{
    return private_job_numbering(process.resources().personality);
}
// Ignored by its action: SIG_IGN, or a default that ignores (CHLD, WINCH, URG)
// or continues (Linux sig_kernel_ignore includes SIGCONT).
bool jobctl_ignored(const job::Numbering &numbering, u32 signal, const sched::SignalAction &action)
{
    if (action.handler == sched::SignalAction::ignored_handler) return true;
    if (action.handler != sched::SignalAction::default_handler) return false;
    const auto kind = job::default_action(numbering, signal);
    return kind == job::DefaultAction::ignore || kind == job::DefaultAction::cont;
}
bool jobctl_blocks(const sched::ThreadControlBlock &thread, u32 signal)
{
    const auto *real = signal_waits.real_blocked(thread);
    return thread.native_signal_mask.contains(signal) || (real && real->contains(signal));
}
bool jobctl_live(const sched::ThreadControlBlock &thread)
{
    return thread.native_active && thread.state != sched::ProcessState::exited;
}

// Kernel signals a Linux handler could not take yet (timers.cpp pattern).
// Bit n is signal n in the recipient's numbering; every kernel signal is < 32.
struct OwedKernelSignals
{
    sched::ProcessId pid{};
    u64 epoch{};
    u32 bits{};
};
std::array<OwedKernelSignals, sched::max_processes> jobctl_owed{};
OwedKernelSignals *jobctl_owed_find(sched::ProcessId pid)
{
    for (auto &entry : jobctl_owed) if (entry.pid == pid && entry.bits) return &entry;
    return nullptr;
}
bool jobctl_owe(const PrivateSignalActions &state, u32 signal)
{
    if (!signal || signal >= 32) return false;
    if (auto *entry = jobctl_owed_find(state.pid))
    {
        if (entry->epoch == state.epoch) { entry->bits |= u32{1} << signal; return true; }
        *entry = {};
    }
    for (auto &entry : jobctl_owed)
        if (!entry.bits)
        {
            entry = {state.pid, state.epoch, u32{1} << signal};
            return true;
        }
    return false;
}

// Linux flush_sigqueue_mask for one signal: shared, every thread and owed.
void jobctl_discard(PrivateSignalActions &state, sched::ProcessControlBlock &process, u32 signal)
{
    state.shared.discard(signal);
    for (auto &thread : process.threads())
        if (jobctl_live(thread))
            if (auto *queue = private_signal_queue(thread)) queue->discard(signal);
    private_jobctl_discard_owed(process.pid(), signal);
}

// CLD_STOPPED/CLD_CONTINUED to a Linux parent; `status` carries the signal in
// the child's numbering. The child-key wakes already came from the overlay.
void jobctl_notify_parent(Kernel &kernel, const sched::ProcessControlBlock &child, sched::ChildExitStatus status)
{
    const auto *parent = kernel.scheduler().find(child.parent_pid());
    if (!parent || parent->resources().personality != Personality::linux) return;
    const auto signal = job::translate(jobctl_numbering(child), jobctl_numbering(*parent),
                                       static_cast<u32>(status.status()));
    if (!signal) return;
    const auto translated = status.code() == 5 ? sched::ChildExitStatus::stopped(signal)
                                               : sched::ChildExitStatus::continued(signal);
    PrivateChildExit::Notification notification{};
    if (auto result = PrivateChildExit::notify_job_state(kernel.scheduler(), child.pid(), translated,
            private_signal_find, [](const sched::ThreadControlBlock &thread) { return signal_waits.real_blocked(thread); },
            notification); !result)
    {
        runtime_failure = result;
        started = false;
    }
}

// Linux stops at dequeue; this kernel stops at generation when `thread` would
// dequeue the stop next: it does not block it (a sigtimedwait takes signals it
// waits for), and no pending signal it would take comes first (directed queue
// before shared, lower number first, ignored entries passed over).
bool jobctl_takes_now(PrivateSignalActions &state, sched::ThreadControlBlock &thread, u32 signal, bool directed)
{
    if (!jobctl_live(thread)) return false;
    auto blocked = thread.native_signal_mask;
    if (signal_waits.real_blocked(thread))
        if (const auto *wait = signal_waits.find(thread))
            for (usize i = 0; i < blocked.words.size(); ++i) blocked.words[i] |= wait->request.wanted.words[i];
    if (blocked.contains(signal)) return false;
    JobPendingView view{private_signal_queue(thread), &state.shared};
    if (!view) return false;
    JobPendingView::Delivery next;
    while (view.peek(blocked, next))
    {
        const auto pending = next.info.signal;
        const auto *action = state.table.find(pending);
        if (action && JobActions::ignored(pending, *action)) { blocked.set(pending); continue; }
        return directed ? next.from_shared || pending > signal : next.from_shared && pending > signal;
    }
    return true;
}

JobSignalOutcome jobctl_enqueue(Kernel &kernel, PrivateSignalActions &state, sched::ProcessControlBlock &recipient,
                                sched::ThreadControlBlock *thread, const sched::PendingSignalInfo &info)
{
    const bool timer = info.code == sched::PendingSignalInfo::timer_code(JobActions::max_signal);
    if (thread)
    {
        if (info.code != 128 && !timer)
        {
            switch (private_signal_wait_enqueue(kernel, recipient, *thread, info))
            {
            case 0: return JobSignalOutcome::accepted;
            case -11: return JobSignalOutcome::full;
            case -12: return JobSignalOutcome::unavailable;
            default: return JobSignalOutcome::invalid;
            }
        }
        auto *queue = private_signal_queue(*thread);
        if (!queue) return JobSignalOutcome::retry;
        const auto result = timer ? queue->enqueue_timer(info.signal, info.timer, info.overrun, info.value)
                                  : queue->enqueue_kernel(info.signal);
        return result == PrivateSignalQueue::Enqueue::queued ||
               (!timer && result == PrivateSignalQueue::Enqueue::coalesced) ? JobSignalOutcome::accepted
                                                                              : JobSignalOutcome::retry;
    }
    if (info.code == 128 || timer)
    {
        // A legacy timer record coalesced into another pending occurrence was
        // not queued; Linux keeps a timer's own record, so it stays owed.
        const auto result = timer ? state.shared.enqueue_timer(info.signal, info.timer, info.overrun, info.value)
                                  : state.shared.enqueue_kernel(info.signal);
        return result == PrivateProcessQueue::Queue::Enqueue::queued ||
               (!timer && result == PrivateProcessQueue::Queue::Enqueue::coalesced) ? JobSignalOutcome::accepted
                                                                                      : JobSignalOutcome::retry;
    }
    const auto result = state.shared.enqueue(info);
    if (result == PrivateProcessQueue::Enqueue::exhausted)
    {
        runtime_failure = Status::fault("private process signal sequence exhausted");
        started = false;
        return JobSignalOutcome::exhausted;
    }
    return result == PrivateProcessQueue::Enqueue::queued || result == PrivateProcessQueue::Enqueue::coalesced
        ? JobSignalOutcome::accepted : JobSignalOutcome::invalid;
}

// Linux kill_orphaned_pgrp at exit, after the exit's own terminal hangups.
void jobctl_orphan_pass(Kernel &kernel, sched::ProcessId pid, sched::ProcessId parent_pid,
                        const PrivateChildExit::Adoption &adoption)
{
    auto &scheduler = kernel.scheduler();
    if (!scheduler.stopped_processes()) return;
    const auto *exited = scheduler.find(pid);
    if (!exited) return;
    const auto group = exited->process_group_id(), session = exited->session_id();
    std::array<sched::ProcessId, sched::max_process_children + 1> groups{};
    usize count{};
    const auto consider = [&](sched::ProcessId candidate, sched::ProcessId ignored) {
        for (usize i = 0; i < count; ++i) if (groups[i] == candidate) return;
        if (count < groups.size() && scheduler.process_group_orphaned(candidate, ignored) &&
            scheduler.process_group_has_stopped(candidate))
            groups[count++] = candidate;
    };
    // The exiting process's own group, while its parent held it.
    if (const auto *parent = scheduler.find(parent_pid);
        parent && parent->process_group_id() != group && parent->session_id() == session)
        consider(group, pid);
    // Each adopted child whose group the exiting process held.
    for (const auto id : adoption.live_children)
        if (const auto *child = scheduler.find(id);
            child && !jobctl_completed(*child) && child->process_group_id() != group && child->session_id() == session)
            consider(child->process_group_id(), 0);
    for (usize i = 0; i < count; ++i)
    {
        static_cast<void>(send_kernel_group_signal(kernel, groups[i], job::native.hup));
        static_cast<void>(send_kernel_group_signal(kernel, groups[i], job::native.cont));
    }
}
} // namespace

bool private_jobctl_discard_stop(Kernel &kernel, const sched::ProcessControlBlock &process, u32 signal)
{
    // Linux get_signal: TSTP, TTIN and TTOU to an orphaned group are dropped,
    // and init (SIGNAL_UNKILLABLE) never stops on a default action.
    return process.pid() == sched::user_init_pid || (signal != JobActions::stop_signal &&
        kernel.scheduler().process_group_orphaned(process.process_group_id()));
}

void private_jobctl_stop(Kernel &kernel, sched::ProcessControlBlock &process, u32 signal)
{
    if (process.job_control().stopped) return;
    if (auto status = kernel.scheduler().stop_process(process.pid(), static_cast<u8>(signal)); !status)
    {
        runtime_failure = status;
        started = false;
        return;
    }
    jobctl_notify_parent(kernel, process, sched::ChildExitStatus::stopped(signal));
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
JobSignalOutcome private_generate_signal(Kernel &kernel, sched::ProcessControlBlock &recipient,
                                         const JobSignalRequest &request)
{
    auto &scheduler = kernel.scheduler();
    const auto pid = recipient.pid();
    // 1. A departed target accepts nothing; a live group needs a live thread.
    if (jobctl_completed(recipient)) return JobSignalOutcome::departed;
    bool live = false;
    for (const auto &thread : recipient.threads()) live |= jobctl_live(thread);
    if (!live) return request.kernel ? JobSignalOutcome::departed : JobSignalOutcome::refused;
    const auto &numbering = jobctl_numbering(recipient);
    const bool linux = recipient.resources().personality == Personality::linux;
    const auto signal = request.info.signal;
    if (!signal || signal > numbering.max) return JobSignalOutcome::invalid;
    auto *state = private_signal_process(kernel, pid);
    if (!state) return JobSignalOutcome::unavailable;
    const auto action = *state->table.find(signal);
    const bool default_action = action.handler == sched::SignalAction::default_handler;
    const auto kind = job::default_action(numbering, signal);
    const bool init = pid == sched::user_init_pid;
    if (!request.replay)
    {
        // 2. Any stop discards a pending SIGCONT, whatever its action.
        if (job::stop_family(numbering, signal)) jobctl_discard(*state, recipient, numbering.cont);
        // 3. SIGCONT discards pending stops and continues, even ignored or blocked.
        else if (signal == numbering.cont)
        {
            for (const auto stop : {numbering.stop, numbering.tstp, numbering.ttin, numbering.ttou})
                jobctl_discard(*state, recipient, stop);
            if (scheduler.continue_process(pid))
                jobctl_notify_parent(kernel, recipient, sched::ChildExitStatus::continued(numbering.cont));
        }
    }
    // 4. SIGKILL acts at the next step, stopped or not; init ignores it.
    if (signal == numbering.kill)
    {
        if (init) return JobSignalOutcome::ignored;
        if (linux) state->group_kill = true;
        else state->deferred_fatal = static_cast<u8>(signal);
        return JobSignalOutcome::accepted;
    }
    auto *named = scheduler.find_thread(pid, request.named ? request.named : pid);
    if (!named && !request.directed)
        for (auto &thread : recipient.threads()) if (jobctl_live(thread)) { named = &thread; break; }
    if (request.directed && (!named || !jobctl_live(*named))) return JobSignalOutcome::departed;
    const bool named_blocks = linux && named && jobctl_blocks(*named, signal);
    // 5. Ignored, unless the named task blocks it (Linux sig_ignored). Init
    // (SIGNAL_UNKILLABLE) ignores terminating and stopping defaults under the
    // same rule; a blocked one is queued and dropped when dequeued.
    const bool ignored = jobctl_ignored(numbering, signal, action);
    if (ignored && !named_blocks) return JobSignalOutcome::ignored;
    if (init && default_action && !named_blocks &&
        (kind == job::DefaultAction::terminate || kind == job::DefaultAction::stop))
        return JobSignalOutcome::ignored;
    // 6. A default stop applies now when deliverable. Linux get_signal drops a
    // deliverable TSTP/TTIN/TTOU in an orphaned group (and any stop of init);
    // otherwise it stays pending for delivery or settle, which apply the same
    // drop. A process already condemned by a fatal signal does not stop.
    const bool stop = default_action && kind == job::DefaultAction::stop;
    if (stop)
    {
        if (state->group_kill || state->deferred_fatal) return JobSignalOutcome::ignored;
        bool now = !linux;
        if (linux && request.directed) now = jobctl_takes_now(*state, *named, signal, true);
        else if (linux)
            for (auto &thread : recipient.threads())
                if ((now = jobctl_takes_now(*state, thread, signal, false))) break;
        if (!now) return jobctl_enqueue(kernel, *state, recipient, request.directed ? named : nullptr, request.info);
        if (private_jobctl_discard_stop(kernel, recipient, signal)) return JobSignalOutcome::ignored;
        private_jobctl_stop(kernel, recipient, signal);
        return JobSignalOutcome::accepted;
    }
    // 7. Native processes never queue: a terminating default is decided now
    // and the first fatal signal wins (KILL above always wins). A stopped
    // native process applies it only after SIGCONT (private_process_signal_fatal).
    if (!linux)
    {
        if (!state->deferred_fatal) state->deferred_fatal = static_cast<u8>(signal);
        return JobSignalOutcome::accepted;
    }
    // 8. Linux: queue for a thread that can take it. A stopped process takes
    // every signal after SIGCONT, so it stays pending (Linux wants_signal).
    // A terminating default that no thread of a running process can take now
    // is the complete_signal fatal shortcut (never for init). CONT and the
    // stops already acted above, so they are queued rather than refused.
    bool unblocked = false, qualified = false;
    for (auto &thread : recipient.threads())
    {
        if (!jobctl_live(thread) || (request.directed && &thread != named)) continue;
        if (thread.native_signal_mask.contains(signal)) continue;
        unblocked = true;
        qualified |= thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running ||
                     private_signal_admit_blocked(kernel, recipient, thread);
    }
    if (!ignored && unblocked && !qualified && !recipient.job_control().stopped)
    {
        if (!init && default_action && kind == job::DefaultAction::terminate)
        {
            if (!state->deferred_fatal) state->deferred_fatal = static_cast<u8>(signal);
            return JobSignalOutcome::accepted;
        }
        const bool acted = !request.replay && (signal == numbering.cont || job::stop_family(numbering, signal));
        if (!acted && !(init && default_action))
            return request.kernel ? JobSignalOutcome::retry : JobSignalOutcome::refused;
    }
    return jobctl_enqueue(kernel, *state, recipient, request.directed ? named : nullptr, request.info);
}

bool private_jobctl_admit_blocked(Kernel &kernel, const sched::ProcessControlBlock &recipient,
                                  const sched::ThreadControlBlock &target, i32 signal)
{
    const auto &numbering = jobctl_numbering(recipient);
    const auto value = static_cast<u32>(signal);
    return recipient.job_control().stopped || value == numbering.kill || value == numbering.cont ||
           job::stop_family(numbering, value) || private_signal_admit_blocked(kernel, recipient, target);
}

KernelSignalSend send_kernel_process_signal(Kernel &kernel, sched::ProcessId pid, u32 signal)
{
    auto *recipient = kernel.scheduler().find(pid);
    if (!recipient || recipient->execution() != sched::ProcessExecution::user_process ||
        recipient->state() == sched::ProcessState::created || jobctl_completed(*recipient))
        return KernelSignalSend::departed;
    const auto translated = job::translate(job::native, jobctl_numbering(*recipient), signal);
    if (!translated) return KernelSignalSend::ignored;
    switch (private_generate_signal(kernel, *recipient, {{translated, 128, 0, 0}, 0, false, true, false}))
    {
    case JobSignalOutcome::accepted: return KernelSignalSend::sent;
    case JobSignalOutcome::ignored: return KernelSignalSend::ignored;
    case JobSignalOutcome::retry:
        if (const auto *state = private_signal_find(pid); state && jobctl_owe(*state, translated))
            return KernelSignalSend::owed;
        return KernelSignalSend::departed;
    default: return KernelSignalSend::departed;
    }
}

KernelGroupSignal send_kernel_group_signal(Kernel &kernel, sched::ProcessId group, u32 signal)
{
    // Snapshot PIDs only; each send may change the process table.
    std::array<sched::ProcessId, sched::max_processes> targets{};
    usize count = 0;
    for (const auto &process : kernel.scheduler().processes())
        if (group && process.process_group_id() == group && count < targets.size()) targets[count++] = process.pid();
    KernelGroupSignal result{};
    result.matched = static_cast<u8>(count);
    for (usize i = 0; i < count; ++i)
        switch (send_kernel_process_signal(kernel, targets[i], signal))
        {
        case KernelSignalSend::sent: ++result.sent; break;
        case KernelSignalSend::ignored: ++result.ignored; break;
        case KernelSignalSend::departed: ++result.departed; break;
        case KernelSignalSend::owed: ++result.owed; break;
        }
    return result;
}

bool private_jobctl_service(Kernel &kernel)
{
    for (auto &entry : jobctl_owed)
    {
        if (!entry.bits) continue;
        auto *process = kernel.scheduler().find(entry.pid);
        const auto *state = private_signal_find(entry.pid);
        if (!process || jobctl_completed(*process) || !state || state->epoch != entry.epoch) { entry = {}; continue; }
        for (u32 signal = 1; signal < 32 && entry.bits; ++signal)
        {
            if (!(entry.bits & (u32{1} << signal))) continue;
            const auto outcome = private_generate_signal(kernel, *process, {{signal, 128, 0, 0}, 0, false, true, true});
            if (outcome != JobSignalOutcome::retry) entry.bits &= ~(u32{1} << signal);
            // The table may have been retired by a fatal decision; re-resolve.
            process = kernel.scheduler().find(entry.pid);
            if (!process || jobctl_completed(*process)) { entry = {}; break; }
        }
        if (!entry.bits) entry = {};
    }
    // A completing wait already causes another step; owed work alone does not.
    return false;
}

void private_jobctl_retire(sched::ProcessId pid)
{
    for (auto &entry : jobctl_owed) if (entry.pid == pid) entry = {};
}

void private_jobctl_discard_owed(sched::ProcessId pid, u32 signal)
{
    if (signal >= 32) return;
    if (auto *entry = jobctl_owed_find(pid))
        if (!(entry->bits &= ~(u32{1} << signal))) *entry = {};
}

bool private_signal_ignored_or_blocked(Kernel &kernel, const sched::ProcessControlBlock &process,
                                       const sched::ThreadControlBlock &thread, u32 signal)
{
    const auto translated = job::translate(job::native, jobctl_numbering(process), signal);
    auto *state = translated ? private_signal_process(kernel, process.pid()) : nullptr;
    if (!state) return false;
    if (state->table.find(translated)->handler == sched::SignalAction::ignored_handler) return true;
    return process.resources().personality == Personality::linux && thread.native_signal_mask.contains(translated);
}

bool private_group_orphaned(Kernel &kernel, sched::ProcessId group)
{
    return kernel.scheduler().process_group_orphaned(group);
}

i64 native_signal_action(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<uptr, 6> &a)
{
    // Order: sigsetsize, act copy, number, KILL/STOP, handler, flags/mask,
    // SIGCHLD SIG_IGN, commit, oact copy (a fault there follows the commit).
    if (a[3] != 8) return -22;
    auto &space = process.resources().native_space;
    ok_sigaction requested{};
    if (a[1] && !hw::copy_from_user(space, &requested, a[1], sizeof(requested))) return -14;
    const auto signal = static_cast<i32>(a[0]);
    if (signal < 1 || signal > OK_NATIVE_SIGNAL_MAX) return -22;
    if (a[1])
    {
        if (signal == OK_SIGKILL || signal == OK_SIGSTOP) return -22;
        if (requested.handler != OK_SIG_DFL && requested.handler != OK_SIG_IGN) return -22;
        if (requested.flags || requested.mask) return -22;
        // No autoreap: refusing beats silently diverging from POSIX.
        if (signal == OK_SIGCHLD && requested.handler == OK_SIG_IGN) return -22;
    }
    auto *state = private_signal_process(kernel, process.pid());
    if (!state) return -12;
    auto &current = *state->table.find(static_cast<u32>(signal));
    const ok_sigaction old{current.handler, 0, 0};
    if (a[1])
    {
        current = {};
        current.handler = requested.handler;
        if (requested.handler == OK_SIG_IGN) private_jobctl_discard_owed(process.pid(), static_cast<u32>(signal));
    }
    if (a[2] && !hw::copy_to_user(space, a[2], &old, sizeof(old))) return -14;
    return 0;
}

void private_jobctl_exec(Kernel &kernel, sched::ProcessControlBlock &process, Personality previous)
{
    const auto &from = private_job_numbering(previous), &to = jobctl_numbering(process);
    if (&from == &to) return;
    auto *state = private_signal_find(process.pid());
    if (!state) return;
    // Exec already reset every action other than SIG_IGN.
    sched::SignalActionTable next{};
    for (u32 signal = 1; signal <= from.max; ++signal)
        if (state->table.find(signal)->handler == sched::SignalAction::ignored_handler)
            if (const auto translated = job::translate(from, to, signal))
                next.find(translated)->handler = sched::SignalAction::ignored_handler;
    state->table = next;
    static_cast<void>(kernel);
}

Status private_exit_commit(Kernel &kernel, sched::ProcessId pid, const PrivateChildExit::Completion &completion,
                           const PrivateChildExit::Adoption &adoption)
{
    if (auto status = PrivateChildExit::commit_adoption(kernel.scheduler(), adoption, private_signal_find); !status)
        return status;
    // The terminal outbox drains here (S4), before the orphan signals (R11).
    jobctl_orphan_pass(kernel, pid, completion.parent, adoption);
    return runtime_failure;
}
#endif
} // namespace runtime_detail
} // namespace ok::user::native
