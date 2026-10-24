// Included by the private serialized signal runtime after queue/delivery setup.
struct PrivateChildSignal
{
    memory::ObjectHandle object{};
    ChildWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};

bool private_child_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivateChildSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        (thread.state != sched::ProcessState::blocked && thread.state != sched::ProcessState::runnable &&
         thread.state != sched::ProcessState::running)) return false;
    const auto object = child_wait_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    if (!syscall_origins.prepare_restart(origin, thread.native_frame, child_origin_binding(object, wait))) return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_child_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivateChildSignal &out)
{
    if (!private_child_signal_owned(kernel, process, thread, out)) return false;
    // Reaping, ECHILD and output admission errors win before caught work.
    if (child_wait_probe(kernel, process.pid(), out.wait.operation) != -11) return false;
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

bool private_child_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivateChildSignal selected;
    return private_child_signal_select(kernel, process, thread, selected);
}

bool private_child_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivateChildSignal selected;
    if (!private_child_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = child_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    const bool restart = (selected.action.flags & sa_restart) != 0;
    bool published = false;
    if (restart)
    {
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, selected.origin, binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged) published = Publication::commit(staged.value(), selected.origin, binding, identity,
                                                        thread.native_frame, capability);
        }
#else
        const auto permit = syscall_origins.prepare_restart(selected.origin, thread.native_frame, binding);
        if (permit && syscall_origins.consume_restart(permit.value(), selected.origin, thread.native_frame, binding))
        { thread.native_frame = permit.value().plan().frame; published = true; }
#endif
    }
    else published = set_result(process, thread.native_frame, -4).ok();
    if (!published)
    { runtime_failure = Status::fault("child wait signal publication lost owned origin"); started = false; return false; }
    // No scheduler or user boundary occurs from selection through handler setup.
    // An already-ready ticket needs no second completion, but its owned request still does.
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("child wait signal lost current wait"); started = false; return false; }
    cancel_child_wait(kernel, selected.object, "interrupted", restart ? 0 : -4);
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_CHILD_SIGNAL pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
        emit(" origin="); emit_number(selected.origin.capture); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(restart ? " outcome=restart" : " outcome=eintr"); emit(" pc="); emit_number(thread.native_frame.pc);
        emit(" selector="); emit_number(static_cast<u32>(wait.operation.selector));
        emit(" output="); emit_number(wait.operation.output); emit(" live=");
        emit_number(child_wait_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
