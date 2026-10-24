// Included by the private serialized signal runtime after queue/delivery setup.
struct PrivatePipeWriteSignal
{
    memory::ObjectHandle object{};
    PipeWriteWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};

bool private_pipe_write_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivatePipeWriteSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        (thread.state != sched::ProcessState::blocked && thread.state != sched::ProcessState::runnable &&
         thread.state != sched::ProcessState::running)) return false;
    const auto object = pipe_write_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    auto resolved = pipe_write_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    if (!wait.origin_capture || origin.capture != wait.origin_capture ||
        !syscall_origins.validate_bound(origin, thread.native_frame, pipe_write_origin_binding(object, wait))) return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_pipe_write_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivatePipeWriteSignal &out)
{
    if (!private_pipe_write_signal_owned(kernel, process, thread, out)) return false;
    const auto ready = kernel.scheduler().pipe_readiness(out.wait.pipe);
    // Sufficient space or a readerless pipe completes normally before delivery.
    if (!ready || !ready.value().readers ||
        (ready.value().available && (out.wait.count > sched::pipe_capacity ||
                                     ready.value().available >= out.wait.count))) return false;
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

bool private_pipe_write_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivatePipeWriteSignal selected;
    return private_pipe_write_signal_select(kernel, process, thread, selected);
}

bool private_pipe_write_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivatePipeWriteSignal selected;
    if (!private_pipe_write_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = pipe_write_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    const bool restart = !wait.committed && (selected.action.flags & sa_restart) != 0;
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
    else published = set_result(process, thread.native_frame, pipe_write_terminal_result(wait, -4)).ok();
    if (!published)
    { runtime_failure = Status::fault("pipe write signal publication lost owned origin"); started = false; return false; }
    // No scheduler or user boundary occurs from selection through handler setup.
    // An already-ready ticket needs no second completion, but its pin still does.
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("pipe write signal lost current wait"); started = false; return false; }
    cancel_pipe_write(kernel, selected.object, "interrupted", restart ? 0 : pipe_write_terminal_result(wait, -4));
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        if (wait.count > sched::pipe_capacity)
            emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_LARGE_SIGNAL pid=" : "OK_NATIVE_PIPE_WRITE_LARGE_SIGNAL pid=");
        else emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_SIGNAL pid=" : "OK_NATIVE_PIPE_WRITE_SIGNAL pid=");
        emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
        emit(" origin="); emit_number(selected.origin.capture); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(wait.committed ? " outcome=partial" : restart ? " outcome=restart" : " outcome=eintr"); emit(" pc="); emit_number(thread.native_frame.pc);
        emit(" syscall="); emit_number(*lu::Codec<private_signal_profile>::syscall_number(wait.vectors.count ? lu::Operation::writev : lu::Operation::write));
        emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " input="); emit_number(wait.input);
        emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors);
        if (wait.count > sched::pipe_capacity) { emit(" committed="); emit_number(wait.committed); }
        emit(" pinned=0 live=");
        emit_number(pipe_write_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
