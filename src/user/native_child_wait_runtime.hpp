// Included inside the serialized native runtime after result encoding.
memory::ObjectHandle child_wait_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : child_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
u64 child_wait_key(const ChildWaitRequest &request)
{ return request.selector == -1 ? 0 : static_cast<u32>(request.selector); }
bool child_wait_output_valid(const sched::ProcessControlBlock &process, const ChildWaitRequest &request)
{
    return !request.siginfo && !request.output ? true :
        hw::user_range(process.resources().native_space, request.output, request.siginfo ? 128 : 4, true);
}
// Return a ready child PID, ECHILD, EFAULT, zero for WNOHANG, or EAGAIN
// exclusively for a matching live child. Never retain a canonical PCB pointer.
i64 child_wait_probe(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request)
{
    const auto *parent = kernel.scheduler().find(pid);
    if (!parent) return -3;
    if (!child_wait_output_valid(*parent, request)) return -14;
    bool eligible = false;
    for (auto id : parent->children())
    {
        if (request.selector != -1 && id != static_cast<u32>(request.selector)) continue;
        const auto *child = kernel.scheduler().find(id);
        if (!child || child->parent_pid() != pid) continue;
        eligible = true;
        if (child->state() == sched::ProcessState::zombie || child->state() == sched::ProcessState::exited)
            return static_cast<i64>(id);
    }
    return !eligible ? -10 : request.nohang ? 0 : -11;
}
void emit_child_wait(const ChildWait &wait, std::string_view outcome, i64 result = -11)
{
#ifdef OK_ENABLE_TEST_POINTS
    emit("OK_NATIVE_CHILD_WAIT pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
    emit(" selector="); emit_number(static_cast<u32>(wait.operation.selector));
    emit(" output="); emit_number(wait.operation.output); emit(wait.operation.siginfo ? " format=siginfo" : " format=status");
    emit(" outcome="); emit(outcome); emit(" result=");
    if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
    else emit_number(static_cast<u64>(result));
    emit(" live="); emit_number(child_wait_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(outcome); static_cast<void>(result);
#endif
}
void cancel_child_wait(Kernel &kernel, memory::ObjectHandle object,
                       std::string_view outcome = "cancelled", i64 result = -125)
{
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    syscall_origins.retire_child(wait.pid, wait.tid, child_origin_binding(object, wait));
    for (auto &entry : child_waits) if (entry.object == object) entry = {};
    // A stale object's retirement cannot cancel a newer wait on this thread.
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (auto status = child_wait_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = child_wait_objects.trim_empty_pages(); !status) runtime_failure = status;
    if (wait.ticket.generation) emit_child_wait(wait, outcome, result);
}
void cancel_child_wait(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : child_waits)
        if (entry.pid == pid) cancel_child_wait(kernel, entry.object);
}
void prune_child_waits(Kernel &kernel)
{
    for (const auto &entry : child_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        const auto resolved = child_wait_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() && (thread->state != sched::ProcessState::blocked ||
             thread->wait_reason != sched::ThreadWaitReason::child || thread->wait_key != child_wait_key(wait.operation))))
        { cancel_child_wait(kernel, object); continue; }
        if (thread->wait_registration.active() && child_wait_probe(kernel, wait.pid, wait.operation) != -11)
            static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    }
}
i64 complete_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, i64 ready)
{
    if (ready < 0) return ready;
    std::array<u8, 128> bytes{};
    auto field = [&](usize offset, u32 value) {
        for (usize i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<u8>(value >> (8 * (request.big_endian ? 3 - i : i)));
    };
    if (ready > 0)
    {
        const auto *child = kernel.scheduler().find(static_cast<sched::ProcessId>(ready));
        if (!child || child->parent_pid() != pid) return -10;
        const u32 status = child->wait_status(), uid = child->credentials().uid;
        if (request.siginfo)
        {
            const u32 signal = status & 0x7fU;
            field(0, 17); field(8, signal ? ((status & 0x80U) ? 3U : 2U) : 1U);
            field(12, static_cast<u32>(ready)); field(16, uid);
            field(20, signal ? signal : ((status >> 8) & 0xffU));
        }
        else field(0, status);
        // Reaping compacts canonical process storage. Re-find the parent for
        // output; a checked copy failure is reported even after consumption.
        auto reaped = kernel.scheduler().wait_process(pid, static_cast<sched::ProcessId>(ready));
        if (!reaped) return error(reaped.status());
    }
    const auto *parent = kernel.scheduler().find(pid);
    if (!parent) return -3;
    if (request.siginfo || (ready && request.output))
        if (!hw::copy_to_user(parent->resources().native_space, request.output, bytes.data(), request.siginfo ? 128 : 4))
            return -14;
    return request.siginfo ? 0 : ready;
}
i64 resume_child_wait(Kernel &kernel, sched::ProcessControlBlock &process, memory::ObjectHandle object, bool &complete)
{
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active() ||
        thread->wait_registration.completion() != sched::WaitCompletion::ready)
    { cancel_child_wait(kernel, object); return -125; }
    const auto ready = child_wait_probe(kernel, wait.pid, wait.operation);
    if (ready == -11)
    {
        // A different waiter may have reaped the wake's child while another
        // eligible child remains. Re-arm the same copied request exactly once.
        const auto before = child_origin_binding(object, wait);
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child, child_wait_key(wait.operation));
        if (!registration) { const auto result = error(registration.status()); cancel_child_wait(kernel, object); return result; }
        wait.ticket = registration.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture && !syscall_origins.rearm(origin, thread->native_frame, before, child_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
        emit_child_wait(wait, "rearm"); complete = false; return -11;
    }
    const auto pid = wait.pid;
    const auto request = wait.operation;
    cancel_child_wait(kernel, object, "complete", ready);
    return complete_child_wait(kernel, pid, request, ready);
}
i64 start_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *thread = kernel.scheduler().current_thread();
    if (!process || !thread || thread->owner != pid || thread->state != sched::ProcessState::running) return -22;
    if (const auto previous = child_wait_object(pid, thread->tid); previous.pool)
        return resume_child_wait(kernel, *process, previous, complete);
    const auto ready = child_wait_probe(kernel, pid, request);
    if (ready != -11) return complete_child_wait(kernel, pid, request, ready);
    PollWaitIndex *slot = nullptr;
    for (auto &entry : child_waits) if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -12;
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return error(status);
#endif
    const auto allocated = child_wait_objects.create();
    if (!allocated) return error(allocated.status());
    const auto object = allocated.value();
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    wait.pid = pid; wait.tid = thread->tid; wait.operation = request;
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child, child_wait_key(request));
    if (!ticket) { cancel_child_wait(kernel, object); return error(ticket.status()); }
    wait.ticket = ticket.value(); wait.request = wait.ticket.generation;
    *slot = {pid, wait.tid, object};
    const auto origin = syscall_origins.identity(*process, *thread);
    if (syscall_origins.live(origin) && !syscall_origins.bind(origin, thread->native_frame, child_origin_binding(object, wait)))
        syscall_origins.revoke(*thread);
    emit_child_wait(wait, "arm"); complete = false; return -11;
}
