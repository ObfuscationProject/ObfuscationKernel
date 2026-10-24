// Included by the native runtime after its wait side table.
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
constexpr bool native_signal_diagnostics = true;
#else
constexpr bool native_signal_diagnostics = false;
#endif

using PrivateSignalWaitState = SignalWaitState<lu::configured_native_profile()>;
void emit(std::string_view text);
void emit_number(u64 value);

usize private_signal_wait_live()
{
    usize count{};
    for (const auto &r : signal_waits.records())
        if (r.phase != PrivateSignalWaitState::Phase::none) ++count;
    return count;
}
void private_signal_wait_identity(const PrivateSignalWaitState::Record &r)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" pid="); emit_number(r.pid); emit(" tid="); emit_number(r.tid);
    emit(" epoch="); emit_number(r.epoch); emit(" wait="); emit_number(r.ticket.generation);
    emit(r.request.kind == SignalWaitKind::suspend ? " kind=suspend" : " kind=timed");
}
void private_signal_wait_inputs(const PrivateSignalWaitState::Record &r)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" syscall="); emit_number(r.syscall); emit(" pc="); emit_number(r.resume);
    emit(" set="); emit_number(r.set_address); emit(" info="); emit_number(r.request.info);
    emit(" timeout="); emit_number(r.timeout_address); emit(" sigsetsize="); emit_number(r.sigsetsize);
    emit(" wanted0="); emit_number(r.request.wanted.words[0]);
    emit(" wanted1="); emit_number(r.request.wanted.words[1]);
    emit(" original0="); emit_number(r.original.words[0]);
    emit(" original1="); emit_number(r.original.words[1]);
    emit(" deadline="); emit_number(r.timeout.deadline());
}
void private_signal_wait_active_mask(const sched::ThreadControlBlock &thread)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" active0="); emit_number(thread.native_signal_mask.words[0]);
    emit(" active1="); emit_number(thread.native_signal_mask.words[1]);
}
void private_signal_wait_signed(i64 value)
{
    if constexpr (!native_signal_diagnostics) return;
    if (value < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(value)); }
    else emit_number(static_cast<u64>(value));
}
void private_signal_wait_trace_retire(const PrivateSignalWaitState::Record &r, std::string_view cause)
{
    if constexpr (!native_signal_diagnostics) return;
    emit("OK_NATIVE_SIGNAL_WAIT_RETIRE"); private_signal_wait_identity(r);
    emit(" cause="); emit(cause); emit(" published=0 staged="); emit_number(r.result_staged);
    emit(" live_before="); emit_number(private_signal_wait_live()); emit("\n");
}
void private_signal_wait_trace_rearm(const PrivateSignalWaitState::Record &r,
                                      sched::WaitTicket old, const sched::ThreadControlBlock &thread,
                                      std::string_view cause)
{
    if constexpr (!native_signal_diagnostics) return;
    emit("OK_NATIVE_SIGNAL_WAIT_REARM"); private_signal_wait_identity(r);
    emit(" old_wait="); emit_number(old.generation); emit(" cause="); emit(cause);
    private_signal_wait_inputs(r); private_signal_wait_active_mask(thread);
    emit(" published=0 staged="); emit_number(r.result_staged);
    emit(" live="); emit_number(private_signal_wait_live()); emit("\n");
}
void private_signal_wait_trace_commit(const sched::ThreadControlBlock &thread, u32 signal)
{
    if constexpr (!native_signal_diagnostics) return;
    if (const auto *r = signal_waits.find(thread); r && signal_waits.saved(thread))
    {
        emit("OK_NATIVE_SIGNAL_WAIT_COMMIT"); private_signal_wait_identity(*r);
        emit(" signal="); emit_number(signal); emit(" published=1 result=-4");
        emit(" live_before="); emit_number(private_signal_wait_live()); emit("\n");
    }
}
