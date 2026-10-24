// Included after private signal delivery and the copied readiness wait runtime.
struct PrivatePollSignal
{
    memory::ObjectHandle object{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
    u64 now{};
};

bool private_poll_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivatePollSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        thread.state != sched::ProcessState::blocked || thread.wait_reason != sched::ThreadWaitReason::poll ||
        thread.wait_key != 0) return false;
    out.object = poll_object(process.pid(), thread.tid);
    if (!out.object.pool) return false;
    auto found = poll_objects.resolve(out.object);
    if (!found) return false;
    auto &wait = *found.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid ||
        wait.ticket.owner != process.pid() || wait.ticket.thread != thread.tid ||
        !thread.wait_registration.pending(wait.ticket) || !wait.syscall ||
        thread.native_frame.pc != wait.resume ||
        poll_syscall_number(thread.native_frame, wait.select_sets) != wait.syscall) return false;
    return true;
}

bool private_poll_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivatePollSignal &out)
{
    if (!private_poll_signal_owned(kernel, process, thread, out)) return false;
    auto found = poll_objects.resolve(out.object);
    if (!found) return false;
    auto &wait = *found.value();
    const auto now = arch::monotonic_nanoseconds();
    if (!now) return false;
    out.now = now.value();
    if (wait.timeout.armed() && out.now >= wait.timeout.deadline()) return false;
    // Provider readiness, EOF and provider errors complete on the normal path.
    const auto ready = poll_ready(kernel, wait);
    if (!ready || ready.value()) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}

bool private_poll_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivatePollSignal selected;
    return private_poll_signal_select(kernel, process, thread, selected);
}

bool private_poll_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivatePollSignal selected;
    if (!private_poll_signal_select(kernel, process, thread, selected)) return false;
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    auto found = poll_objects.resolve(selected.object);
    if (!found) return false;
    const auto &wait = *found.value();
    const auto ticket = wait.ticket;
    const auto deadline = wait.timeout.deadline();
    const bool select_sets = wait.select_sets;
    if (!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::interrupted)) return false;
    // Poll writes only revents on EINTR; select leaves the supplied sets intact.
    // Neither family restarts for a caught handler, even with SA_RESTART.
    const i64 result = !select_sets && !poll_output(process.resources().native_space, wait) ? -14 : -4;
    finish_poll_timeout(process.resources().native_space, wait.timeout_output, selected.now);
    const auto published = set_result(process, thread.native_frame, result);
    thread.syscall_pending = false;
    // EFAULT is an ordinary result and restores before subsequent selection.
    // EINTR leaves the temporary delivery mask live until a frame commits.
    poll_signal_masks.complete(thread, selected.object, ticket, result == -4);
    cancel_poll(kernel, selected.object, "interrupted", result);
    if (!published || !runtime_failure)
    {
        if (!published) runtime_failure = published;
        started = false;
        return false;
    }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_POLL_SIGNAL pid="); emit_number(process.pid()); emit(" tid="); emit_number(thread.tid);
        emit(" wait="); emit_number(ticket.generation); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(select_sets ? " kind=select" : " kind=poll"); emit(result == -14 ? " result=efault" : " result=eintr");
        emit(" deadline="); emit_number(deadline); emit(" now="); emit_number(selected.now);
        emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
