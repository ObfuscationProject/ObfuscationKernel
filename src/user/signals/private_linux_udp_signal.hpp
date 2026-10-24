// Included after captured Linux receive and private signal implementations.
#if defined(OK_ENABLE_TEST_POINTS)
constexpr bool linux_udp_signal_diagnostics = true;
#else
constexpr bool linux_udp_signal_diagnostics = false;
#endif
struct PrivateLinuxUdpSignal
{
    memory::ObjectHandle object{};
    SyscallOrigins::LinuxUdpReceiveBinding binding{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
#ifdef OK_LINUX_UDP_SIGNAL_FAILURE_FIXTURE
#if !defined(OK_SYSCALL_ORIGIN_MODEL) || defined(OK_KERNEL_FREESTANDING)
#error "Linux UDP signal failure controls require the hosted capture model"
#endif
// A scalar hosted input exercises the real encoder after exact consumption.
// It cannot replace capture, publication authority or a scheduling boundary.
i64 private_linux_udp_signal_model_result{-4};
#endif

bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread, PrivateLinuxUdpSignal &out)
{
    if (!runtime_failure || kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited)
        return false;
    const auto object = linux_udp_receive_object(process.pid(), thread.tid);
    if (object.pool != &linux_udp_receive_objects) return false;
    const auto *member = linux_udp_receive_member(object);
    const auto resolved = member ? linux_udp_receive_objects.resolve(object)
        : Result<LinuxUdpReceiveWait *>{Status::denied("Linux UDP signal lost membership")};
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    const auto cause = thread.wait_registration.active() ? sched::WaitCompletion::none
                                                         : thread.wait_registration.completion();
    if (member->pid != process.pid() || member->tid != thread.tid || wait.pid != process.pid() ||
        wait.tid != thread.tid || wait.phase != LinuxUdpReceivePhase::published || !wait.capture || !wait.binding ||
        *wait.binding != linux_udp_receive_binding(object, wait) ||
        syscall_origins.linux_udp_receive_borrowed(*wait.capture) ||
        !syscall_origins.validate_linux_udp_receive(*wait.capture, thread.native_frame, *wait.binding, cause))
        return false;
    out.object = object; out.binding = *wait.binding;
    return true;
}

bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal owned;
    return private_linux_udp_signal_owned(kernel, process, thread, owned);
}

bool private_linux_udp_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread, PrivateLinuxUdpSignal &out)
{
    if (!private_linux_udp_signal_owned(kernel, process, thread, out)) return false;
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), out.binding.endpoint);
    const auto *description = kernel.scheduler().open_description(out.binding.description);
    // Actual data and terminal results keep normal receive priority. Sequence
    // changes without remaining data do not make a stolen readiness wake final.
    if (!ready || ready.value().receive.readable() || ready.value().receive.terminal ||
        ready.value().executor.failed || !description || (description->flags & posix::o_NONBLOCK)) return false;
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

bool private_linux_udp_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal selected;
    return private_linux_udp_signal_select(kernel, process, thread, selected);
}

bool private_linux_udp_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                       sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal selected;
    if (!private_linux_udp_signal_select(kernel, process, thread, selected)) return false;
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    const auto resolved = linux_udp_receive_objects.resolve(selected.object);
    if (!resolved) { static_cast<void>(linux_udp_receive_invariant(resolved.status())); return false; }
    auto &wait = *resolved.value();
    const auto qualified = syscall_origins.linux_udp_receive_restart_qualified(
        *wait.capture, thread.native_frame, selected.binding);
    if (!qualified) { static_cast<void>(linux_udp_receive_invariant(qualified.status())); return false; }
    constexpr u64 sa_restart = 0x10000000;
    // Only a valid exact record with absent opcode qualification can use the
    // compatibility EINTR fallback. Stale authority never selects this branch.
    const bool restart = (selected.action.flags & sa_restart) && qualified.value();
    // Diagnostic scalars are never publication authority. A failed lookup
    // leaves zeros and cannot alter the original completion decision.
    u64 trace_capture{}, trace_epoch{}, trace_image{}, trace_stack_generation{};
    if constexpr (linux_udp_signal_diagnostics)
    {
        const auto identity = syscall_origins.linux_udp_receive_identity(
            *wait.capture, thread.native_frame, selected.binding);
        if (identity)
        {
            trace_capture = identity.value().capture;
            trace_epoch = identity.value().epoch;
            trace_image = identity.value().image;
            trace_stack_generation = identity.value().lease.generation;
        }
    }
    Status published{};
    if (restart)
    {
        const auto origin = syscall_origins.linux_udp_receive_identity(*wait.capture, thread.native_frame, selected.binding);
        if (!origin) { static_cast<void>(linux_udp_receive_invariant(origin.status())); return false; }
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
        published = Status::fault("Linux UDP restart publication lacks readiness");
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, origin.value(), selected.binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged && Publication::commit(staged.value(), origin.value(), selected.binding, identity,
                                              thread.native_frame, capability)) published = {};
        }
#else
        const auto permit = syscall_origins.prepare_restart(origin.value(), thread.native_frame, selected.binding);
        if (!permit) { static_cast<void>(linux_udp_receive_invariant(permit.status())); return false; }
        if (!syscall_origins.consume_restart(permit.value(), origin.value(), thread.native_frame, selected.binding))
            published = Status::fault("Linux UDP restart publication lost exact permit");
        else thread.native_frame = permit.value().plan().frame;
#endif
    }
    else
    {
        published = syscall_origins.consume_linux_udp_receive_interruption(
            *wait.capture, thread.native_frame, selected.binding);
        if (!published) { static_cast<void>(linux_udp_receive_invariant(published)); return false; }
        // EINTR is 4 on all ten target profiles, independent of opcode proof.
        published = set_linux_udp_result(process, thread.native_frame,
#ifdef OK_LINUX_UDP_SIGNAL_FAILURE_FIXTURE
        private_linux_udp_signal_model_result
#else
        -4
#endif
        );
    }
    if (!published) static_cast<void>(linux_udp_receive_invariant(published));
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(selected.binding.ticket, sched::WaitCompletion::interrupted))
    {
        const auto failed_ticket = Status::fault("Linux UDP signal lost its exact current ticket");
        if (published) published = failed_ticket;
        static_cast<void>(linux_udp_receive_invariant(failed_ticket));
    }
    const auto retired = retire_linux_udp_receive(kernel, selected.object, LinuxUdpReceiveRetirement::completed);
    thread.syscall_pending = false;
    if constexpr (linux_udp_signal_diagnostics)
    {
        // Emit only after all original success conditions, before user entry.
        // No wait/object/capture reference is read after exact retirement.
        if (published && retired && runtime_failure)
        {
            emit("OK_LINUX_UDP_SIGNAL pid="); emit_number(process.pid());
            emit(" tid="); emit_number(thread.tid);
            emit(" capture="); emit_number(trace_capture);
            emit(" epoch="); emit_number(trace_epoch);
            emit(" image="); emit_number(trace_image);
            emit(" stack_generation="); emit_number(trace_stack_generation);
            emit(" object_page="); emit_number(selected.object.page_slot);
            emit(" object_slot="); emit_number(selected.object.object_slot);
            emit(" object_generation="); emit_number(selected.object.generation);
            emit(" request="); emit_number(selected.binding.request_sequence);
            emit(" ticket="); emit_number(selected.binding.ticket.generation);
            emit(" signal="); emit_number(selected.pending.info.signal);
            emit(" qualified="); emit_number(qualified.value() ? 1 : 0);
            emit(restart ? " outcome=restart" : " outcome=eintr");
            emit(" pc="); emit_number(thread.native_frame.pc);
            emit(" retired=1 pending=0\n");
        }
    }
    return published && retired && runtime_failure;
}
