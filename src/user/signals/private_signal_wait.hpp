// Included after queue/delivery types and before private syscall interception.
using PrivateSignalWaitCodec = SignalWaitCodec<private_signal_profile>;

bool private_signal_wait_frame(const sched::ThreadControlBlock &thread,
                               const PrivateSignalWaitState::Record &record)
{
    if (thread.native_frame.pc != record.resume) return false;
    if (!record.result_staged) return hw::syscall(thread.native_frame).number == record.syscall;
    const auto result = lu::Codec<private_signal_profile>::decode_return(
        lu::NativeBridge<private_signal_profile>::snapshot(thread.native_frame));
    return record.request.kind == SignalWaitKind::suspend && result && result.value().error == 4;
}
bool private_signal_wait_owner(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    const auto *r = signal_waits.find(thread);
    return r && r->phase == PrivateSignalWaitState::Phase::waiting &&
        kernel.scheduler().find(process.pid()) == &process &&
        kernel.scheduler().find_thread(process.pid(), thread.tid) == &thread &&
        process.pid() == thread.owner && process.resources().personality == Personality::linux &&
        process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited &&
        thread.native_active && thread.syscall_pending && private_signal_wait_frame(thread, *r) &&
        thread.wait_registration.ticket() == r->ticket;
}
bool private_signal_wait_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    if (!private_signal_wait_owner(kernel, process, thread)) return false;
    const auto &r = *signal_waits.find(thread);
    const auto reason = r.request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                              : sched::ThreadWaitReason::signal_suspend;
    return thread.state == sched::ProcessState::blocked && thread.wait_reason == reason &&
        thread.wait_key == r.epoch && thread.wait_registration.pending(r.ticket);
}
enum class PrivateSignalWaitOutcome : u8 { none, received, timeout, interrupted, ignored, cancelled, rearm, fatal };
struct PrivateSignalWaitSelection
{
    PrivateSignalWaitOutcome outcome{};
    PrivateSignalView::Delivery pending{};
};
PrivateSignalWaitSelection private_signal_wait_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                                       const sched::ThreadControlBlock &thread, u64 now)
{
    if (!private_signal_wait_owner(kernel, process, thread)) return {};
    const auto &r = *signal_waits.find(thread);
    if (!thread.wait_registration.active())
    {
        // A committed external cancellation owns the outcome. A generic ready
        // wake only rearms the copied logical request; it consumes no signal.
        if (thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running)
            return {thread.wait_registration.completion() == sched::WaitCompletion::ready
                ? PrivateSignalWaitOutcome::rearm : PrivateSignalWaitOutcome::cancelled, {}};
        return {};
    }
    if (!private_signal_wait_owned(kernel, process, thread)) return {};
    auto pending = private_signal_pending(kernel, process, thread);
    if (!pending) return {};
    PrivateSignalWaitSelection selected;
    if (r.request.kind == SignalWaitKind::timed)
    {
        if (pending.peek(PrivateSignalWaitCodec::selection_mask(r.request.wanted), selected.pending))
        { selected.outcome = PrivateSignalWaitOutcome::received; return selected; }
        // Matching work wins at the final dequeue even when the timer elapsed.
        // With no match, the serialized elapsed timer wins unrelated interrupts.
        if (r.timeout.armed() && now >= r.timeout.deadline())
            return {PrivateSignalWaitOutcome::timeout, {}};
    }
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!actions) return {};
    auto blocked = thread.native_signal_mask;
    while (pending.peek(blocked, selected.pending))
    {
        const auto *action = actions->find(selected.pending.info.signal);
        if (!action) return {};
        if (PrivateSignalDelivery::Actions::ignored(selected.pending.info.signal, *action))
        {
            if (r.request.kind == SignalWaitKind::suspend)
            { selected.outcome = PrivateSignalWaitOutcome::ignored; return selected; }
            // Linux's raw timedwait wakes for unrelated unblocked ignored
            // work. Normal return delivery drains it without a handler frame.
            selected.outcome = PrivateSignalWaitOutcome::interrupted; return selected;
        }
        if (action->handler == sched::SignalAction::default_handler)
        { selected.outcome = PrivateSignalWaitOutcome::fatal; return selected; }
        if (!PrivateSignalDelivery::supported(*action)) return {};
        selected.outcome = PrivateSignalWaitOutcome::interrupted; return selected;
    }
    return {};
}
bool private_signal_wait_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, u64 now)
{
    return private_signal_wait_select(kernel, process, thread, now).outcome != PrivateSignalWaitOutcome::none;
}
i64 private_signal_wait_copy(sched::ProcessControlBlock &process, PrivateSignalView &queue,
                             const PrivateSignalView::Delivery &ticket, u64 output)
{
    std::array<std::byte, 128> info{};
    if (!PrivateSignalWaitCodec::encode_info(ticket.info, info) || !queue.consume(ticket))
    {
        runtime_failure = Status::fault("signal wait lost selected queue authority");
        started = false; return -5;
    }
    // Linux synchronous receive is destructive before its optional copyout.
    // Do not validate the output while empty or undo this consume on EFAULT.
    return output && !hw::copy_to_user(process.resources().native_space, output, info.data(), info.size())
        ? -14 : static_cast<i64>(ticket.info.signal);
}
bool private_signal_wait_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread, u64 now)
{
    auto selected = private_signal_wait_select(kernel, process, thread, now);
    using Outcome = PrivateSignalWaitOutcome;
    while (selected.outcome == Outcome::ignored)
    {
        // Like Linux get_signal, suspend drains unblocked ignored occurrences
        // without returning to userspace or restoring its temporary mask.
        auto queue = private_signal_pending(kernel, process, thread);
        if (!queue.consume(selected.pending))
        { runtime_failure = Status::fault("suspend ignored dequeue lost owner"); started = false; return false; }
        selected = private_signal_wait_select(kernel, process, thread, now);
    }
    if (selected.outcome == Outcome::none) return false;
    if (selected.outcome == Outcome::rearm)
    {
        auto &r = *signal_waits.find(thread);
        const auto reason = r.request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                                  : sched::ThreadWaitReason::signal_suspend;
        if (!kernel.scheduler().activate_thread(process.pid(), thread.tid))
        { runtime_failure = Status::fault("signal wait rearm activation failed"); started = false; return false; }
        const auto ticket = kernel.scheduler().register_current_wait(reason, r.epoch);
        if (!ticket)
        { runtime_failure = ticket.status(); started = false; return false; }
        const auto old = r.ticket; r.ticket = ticket.value();
        private_signal_wait_trace_rearm(r, old, thread, "ready");
        return false; // The logical wait still blocks; never enter user mode.
    }
    if (selected.outcome == Outcome::fatal)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    const auto record = *signal_waits.find(thread);
    const auto cause = selected.outcome == Outcome::received ? sched::WaitCompletion::ready :
                       selected.outcome == Outcome::timeout ? sched::WaitCompletion::timeout :
                       sched::WaitCompletion::interrupted;
    if (selected.outcome != Outcome::cancelled && !kernel.scheduler().complete_wait(record.ticket, cause)) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const bool suspend_delivery = record.request.kind == SignalWaitKind::suspend && selected.outcome == Outcome::interrupted;
    if (suspend_delivery)
    {
        signal_waits.find(thread)->phase = PrivateSignalWaitState::Phase::delivery;
        signal_waits.find(thread)->result_staged = true;
    }
    else signal_waits.restore(thread);
    i64 result = selected.outcome == Outcome::timeout ? -11 : -4;
    if (selected.outcome == Outcome::received)
        result = private_signal_wait_copy(process, queue, selected.pending, record.request.info);
    thread.syscall_pending = false;
    // A suspend rearmed after frame deferral already has a verified EINTR
    // completion. PPC's completion proof is one-shot; do not republish it.
    const auto published = record.result_staged ? Status::success() : set_result(process, thread.native_frame, result);
    if (!published || !runtime_failure)
    {
        if (!published) runtime_failure = published;
        started = false; return false;
    }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_SIGNAL_WAIT_COMPLETE"); private_signal_wait_identity(record);
        emit(" outcome=");
        emit(selected.outcome == Outcome::received ? (result == -14 ? "efault" : "received") :
             selected.outcome == Outcome::timeout ? "timeout" : selected.outcome == Outcome::cancelled ? "cancelled" : "eintr");
        emit(" signal="); emit_number(selected.pending.info.signal);
        emit(" code="); private_signal_wait_signed(selected.pending.info.code);
        emit(" sender_pid="); emit_number(selected.pending.info.sender_pid);
        emit(" sender_uid="); emit_number(selected.pending.info.sender_uid);
        emit(" info="); emit_number(record.request.info);
        emit(" result="); private_signal_wait_signed(result);
        emit(" published="); emit_number(!suspend_delivery);
        emit(" staged="); emit_number(suspend_delivery);
        emit(" consumed="); emit_number(selected.outcome == Outcome::received);
        emit(" live="); emit_number(private_signal_wait_live());
        emit(" deadline="); emit_number(record.timeout.deadline()); emit(" now="); emit_number(now); emit("\n");
    }
    return true;
}
bool private_signal_wait_call(Kernel &kernel, sched::ProcessControlBlock &process,
                               sched::ThreadControlBlock &thread, bool &complete, i64 &result)
{
    const auto raw = hw::syscall(thread.native_frame);
    if (PrivateSignalWaitCodec::kind(raw.number) == SignalWaitKind::none) return false;
    const auto decoded = lu::Codec<private_signal_profile>::decode_call(
        lu::NativeBridge<private_signal_profile>::snapshot(thread.native_frame), 4);
    if (!decoded) { result = -22; return true; }
    PrivateSignalWaitCodec::Request request;
    result = PrivateSignalWaitCodec::decode(decoded.value(),
        [&](u64 address, std::span<std::byte> bytes) {
            return hw::copy_from_user(process.resources().native_space, bytes.data(), address, bytes.size()).ok();
        }, request);
    if (result) return true;
    if (signal_waits.find(thread) || poll_signal_masks.find(thread)) { result = -22; return true; }
    auto queue = private_signal_pending(kernel, process, thread);
    if (!queue) { result = -12; return true; }
    if (request.kind == SignalWaitKind::timed)
    {
        PrivateSignalView::Delivery ticket;
        if (queue.peek(PrivateSignalWaitCodec::selection_mask(request.wanted), ticket))
        { result = private_signal_wait_copy(process, queue, ticket, request.info); return true; }
        if (request.has_timeout && request.timeout.seconds == 0 && request.timeout.nanoseconds == 0)
        { result = -11; return true; }
    }
    u64 now{};
    if (request.kind == SignalWaitKind::timed && request.has_timeout)
    {
        const auto clock = arch::monotonic_nanoseconds();
        if (!clock) { result = error(clock.status()); return true; }
        now = clock.value();
    }
    const auto &arguments = decoded.value().arguments;
    const std::array<u64, 3> input{arguments[0], request.kind == SignalWaitKind::timed ? arguments[2] : 0,
                                 arguments[request.kind == SignalWaitKind::timed ? 3 : 1]};
    if (!signal_waits.begin(thread, request, raw.number, now, input)) { result = -12; return true; }
    const auto reason = request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                            : sched::ThreadWaitReason::signal_suspend;
    const auto ticket = kernel.scheduler().register_current_wait(reason, signal_waits.find(thread)->epoch);
    if (!ticket)
    { signal_waits.restore(thread); result = error(ticket.status()); return true; }
    if (!signal_waits.bind(thread, ticket.value()))
    {
        static_cast<void>(kernel.scheduler().complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
        signal_waits.restore(thread); runtime_failure = Status::fault("signal wait bind lost owner");
        started = false; result = -5; return true;
    }
    complete = false; result = 0;
    const auto &record = *signal_waits.find(thread);
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_SIGNAL_WAIT_ARM"); private_signal_wait_identity(record);
        private_signal_wait_inputs(record); private_signal_wait_active_mask(thread);
        emit(" live="); emit_number(private_signal_wait_live()); emit("\n");
    }
    return true;
}
bool private_signal_wait_pending(Kernel &kernel)
{
    for (const auto &record : signal_waits.records())
    {
        if (record.phase == PrivateSignalWaitState::Phase::none) continue;
        const auto *owner = kernel.scheduler().find_thread(record.pid, record.tid);
        if (owner && owner->native_active && signal_waits.find(*owner) == &record) return true;
    }
    return false;
}
bool private_signal_wait_rearm_delivery(Kernel &kernel, sched::ProcessControlBlock &process,
                                        sched::ThreadControlBlock &thread)
{
    auto *r = signal_waits.find(thread);
    if (!r || !signal_waits.saved(thread)) return false;
    // A deferred architecture frame can lose its pending handler when a peer
    // commits SIG_IGN. Keep the original temporary mask and logical suspend;
    // the already staged EINTR becomes observable only in an eventual frame.
    if (!kernel.scheduler().activate_thread(process.pid(), thread.tid))
    { runtime_failure = Status::fault("suspend delivery rearm activation failed"); started = false; return false; }
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::signal_suspend, r->epoch);
    if (!ticket)
    { runtime_failure = ticket.status(); started = false; return false; }
    const auto old = r->ticket;
    r->ticket = ticket.value(); r->phase = PrivateSignalWaitState::Phase::waiting;
    thread.syscall_pending = true;
    private_signal_wait_trace_rearm(*r, old, thread, "delivery");
    return false; // The caller must not enter user mode after rearming.
}
i64 private_signal_wait_enqueue(Kernel &kernel, sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target,
                                 sched::PendingSignalInfo info)
{
    const auto *real = signal_waits.real_blocked(target);
    if (real && real->contains(info.signal))
    {
        // The existing public helper checks only the live delivery mask.
        // A sigtimedwait original mask retains ignored signals at generation.
        auto *queue = private_signal_queue(target);
        if (!queue) return -12;
        const auto result = queue->enqueue(info);
        return result == PrivateSignalQueue::Enqueue::queued || result == PrivateSignalQueue::Enqueue::coalesced ? 0 : -11;
    }
    return user::enqueue_directed_signal<private_signal_profile>(recipient, target, info,
        [&](sched::ProcessId pid) { return private_signal_table(kernel, pid); },
        [&](sched::ThreadControlBlock &owner) { return private_signal_queue(owner); });
}
