// Actual runtime signal generation, stop/continue, waits and orphans with
// hosted trap/mapping backends. Hosted evidence only; no guest execution.
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"

namespace
{
using N = syscall::Number;
using Codec = native::SignalActionCodec<lu::Profile::x86_64>;
constexpr uptr status_at = hw::user_begin + 384, act_at = hw::user_begin + 448, old_at = hw::user_begin + 480;
constexpr uptr event_at = hw::user_begin + 576, spec_at = hw::user_begin + 640, id_at = hw::user_begin + 672;
constexpr u64 marker = 0x51ab1e;
usize native_calls{}, linux_calls{}, profiles{};

u64 negative(u64 group) { return static_cast<u64>(-static_cast<i64>(group)); }
i64 native_call(Kernel &kernel, u64 pid, N number, std::array<uptr, 6> arguments)
{
    activate(kernel, pid, pid);
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    expect(complete);
    ++native_calls;
    return result;
}
i64 native_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal)
{ return native_call(kernel, sender, N::kill, {selector, signal}); }
i64 linux_call(Kernel &kernel, u64 pid, lu::Operation operation, std::array<u64, 4> a,
               std::source_location at = std::source_location::current())
{
    activate(kernel, pid, pid);
    auto frame = kernel.scheduler().find_thread(pid, pid)->native_frame;
    frame.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(operation);
    frame.registers[9] = a[0]; frame.registers[10] = a[1]; frame.registers[11] = a[2]; frame.registers[5] = a[3];
    bool complete = true;
    i64 result = -999;
    if (operation == lu::Operation::wait4)
        result = user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel, pid, frame, complete,
            [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, pid, request, done); });
    else
    {
        auto &thread = *kernel.scheduler().find_thread(pid, pid);
        const auto saved = thread.native_frame;
        thread.native_frame = frame;
        expect(native::private_signal_call(kernel, *kernel.scheduler().find(pid), thread, complete, result), at);
        thread.native_frame = saved;
    }
    expect(complete, at);
    ++linux_calls;
    return result;
}
i64 linux_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal,
               std::source_location at = std::source_location::current())
{ return linux_call(kernel, sender, lu::Operation::kill, {selector, signal}, at); }
u32 status_word()
{
    u32 value = 0;
    for (usize i = 0; i < 4; ++i) value |= std::to_integer<u32>(mock::memory[status_at - hw::user_begin + i]) << (8 * i);
    return value;
}
void put64(uptr address, u64 value)
{
    for (usize i = 0; i < 8; ++i) mock::memory[address - hw::user_begin + i] = static_cast<std::byte>(value >> (8 * i));
}
// A one-shot POSIX timer of `pid` (x86_64 sigevent): `signal`, process-directed
// or to `tid`, armed TIMER_ABSTIME at 1 ns so the next service expires it.
u32 arm_timer(Kernel &kernel, u64 pid, u32 signal, u32 tid = 0)
{
    for (usize i = 0; i < 64; ++i) mock::memory[event_at - hw::user_begin + i] = std::byte{};
    put64(event_at, 0x77); put64(event_at + 8, signal | (u64{tid ? 4U : 0U} << 32)); put64(event_at + 16, tid);
    expect(linux_call(kernel, pid, lu::Operation::timer_create, {1, event_at, id_at}) == 0);
    const auto id = static_cast<u32>(mock::memory[id_at - hw::user_begin]);
    put64(spec_at, 0); put64(spec_at + 8, 0); put64(spec_at + 16, 0); put64(spec_at + 24, 1);
    expect(linux_call(kernel, pid, lu::Operation::timer_settime, {id, 1, spec_at, 0}) == 0);
    return id;
}
void timer_service(Kernel &kernel)
{
    static_cast<void>(native::private_timer_service(kernel, 1000000));
    expect(native::runtime_status().ok());
}
u64 get64(uptr address)
{
    u64 value = 0;
    for (usize i = 0; i < 8; ++i) value |= std::to_integer<u64>(mock::memory[address - hw::user_begin + i]) << (8 * i);
    return value;
}
void grouped(Kernel &kernel, u64 parent, u64 child) { CHECK(kernel.scheduler().set_process_group(parent, static_cast<i32>(child), static_cast<i32>(child))); }
void handle(Kernel &kernel, u64 pid, u32 signal, u64 disposition = handler, u64 flags = 0)
{
    auto *entry = native::private_signal_table(kernel, pid)->find(signal); CHECK(entry);
    *entry = {}; entry->handler = disposition; entry->flags = flags;
    if (disposition > 1) { entry->flags |= 0x04000004; entry->restorer = handler + 4; }
}
const sched::JobControlState &job(Kernel &kernel, u64 pid) { return kernel.scheduler().find(pid)->job_control(); }
native::PrivateProcessQueue &shared(u64 pid) { return native::private_signal_find(pid)->shared; }
// The parent's pending SIGCHLD with its CLD code and status, consumed.
void expect_child_signal(u64 parent, u64 child, i32 code, i32 status)
{
    native::PrivateProcessQueue::Delivery ticket;
    expect(shared(parent).peek({}, ticket) && ticket.info.signal == sigchld && ticket.info.code == code &&
           ticket.info.sender_pid == child && ticket.info.child_status == status);
    expect(shared(parent).consume(ticket));
}
void drain(u64 pid)
{
    native::PrivateProcessQueue::Delivery ticket;
    while (shared(pid).peek({}, ticket)) CHECK(shared(pid).consume(ticket));
}
// Wait in a console read, which no signal interrupts.
sched::WaitTicket park(Kernel &kernel, u64 pid)
{
    activate(kernel, pid, pid);
    auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read, 0); CHECK(ticket);
    kernel.scheduler().find_thread(pid, pid)->syscall_pending = true;
    return ticket.value();
}
void unpark(Kernel &kernel, u64 pid, sched::WaitTicket ticket)
{
    CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    auto *thread = kernel.scheduler().find_thread(pid, pid);
    thread->syscall_pending = false;
    if (thread->state == sched::ProcessState::blocked) thread->state = sched::ProcessState::runnable;
}

template <lu::Profile P> void numbering()
{
    using C = native::SignalActionCodec<P>;
    constexpr const auto &linux = user::job::linux_numbering<P>;
    static_assert(C::stop_signal == linux.stop && C::continue_signal == linux.cont && C::tstp_signal == linux.tstp);
    for (u32 signal : {u32{OK_SIGHUP}, u32{OK_SIGINT}, u32{OK_SIGCHLD}, u32{OK_SIGCONT}, u32{OK_SIGSTOP},
                       u32{OK_SIGTSTP}, u32{OK_SIGTTIN}, u32{OK_SIGTTOU}, u32{OK_SIGWINCH}})
    {
        const auto translated = user::job::translate(user::job::native, linux, signal);
        expect(translated && user::job::translate(linux, user::job::native, translated) == signal);
        expect(user::job::default_action(linux, translated) == user::job::default_action(user::job::native, signal));
    }
    expect(user::job::translate(user::job::native, linux, OK_SIGSTOP) == C::stop_signal);
    // MIPS EMT and generic STKFLT have no counterpart: kill gives EINVAL.
    if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
        expect(!user::job::translate(linux, user::job::native, 7) && !user::job::translate(user::job::native, linux, 16));
    ++profiles;
}

// Linux kill/tkill/group stop and continue, the mutual discards, waits on
// selectors 0 and -pgid, the stopped thread never entered, KILL while stopped.
void linux_stop_continue(Kernel &kernel)
{
    const auto parent = create(kernel);
    action(kernel, parent, handler);
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    kernel.scheduler().find_thread(child, child)->native_frame.registers[0] = marker;
    // TSTP stops at generation and the parent gets CLD_STOPPED.
    expect(linux_kill(kernel, parent, child, 20) == 0);
    expect(job(kernel, child).stopped && job(kernel, child).stop_signal == 20 && job(kernel, child).stop_report);
    expect(kernel.scheduler().stopped_processes() == 1 && !shared(child).pending_mask().contains(20));
    expect_child_signal(parent, child, 5, 20);
    // The stopped thread is never entered while the others run.
    native::started = true; native::last_pid = 0; native::last_tid = 0;
    usize entered = 0;
    for (unsigned i = 0; i < 6; ++i)
    {
        mock::last_frame = {};
        const auto entries = mock::entries;
        static_cast<void>(native::step(kernel));
        expect(native::runtime_status().ok());
        entered += mock::entries != entries;
        expect(mock::last_frame.registers[0] != marker);
    }
    expect(entered > 0);
    // Selector 0 is the parent's own group: the child left it.
    expect(linux_call(kernel, parent, lu::Operation::wait4, {0, status_at, 3, 0}) == -10);
    expect(linux_call(kernel, parent, lu::Operation::wait4, {negative(child), status_at, 3, 0}) == static_cast<i64>(child));
    expect(status_word() == ((20U << 8) | 0x7f) && !job(kernel, child).stop_report && job(kernel, child).stopped);
    expect(linux_call(kernel, parent, lu::Operation::wait4, {negative(child), status_at, 3, 0}) == 0);
    // A blocked CONT continues anyway and stays queued; a stop then discards it.
    auto &thread = *kernel.scheduler().find_thread(child, child);
    thread.native_signal_mask.set(18);
    expect(linux_kill(kernel, parent, negative(child), 18) == 0);
    expect(!job(kernel, child).stopped && shared(child).pending_mask().contains(18));
    expect_child_signal(parent, child, 6, 18);
    expect(linux_call(kernel, parent, lu::Operation::wait4, {negative(child), status_at, 9, 0}) == static_cast<i64>(child));
    expect(status_word() == 0xffffU && !job(kernel, child).continue_report);
    expect(linux_call(kernel, parent, lu::Operation::tkill, {child, 19}) == 0);
    expect(job(kernel, child).stopped && !shared(child).pending_mask().contains(18));
    expect_child_signal(parent, child, 5, 19);
    // A blocked TSTP stays pending while stopped; CONT discards it.
    thread.native_signal_mask = {}; thread.native_signal_mask.set(20);
    expect(linux_kill(kernel, parent, child, 20) == 0 && shared(child).pending_mask().contains(20));
    expect(linux_kill(kernel, parent, child, 18) == 0 && !shared(child).pending_mask().contains(20));
    expect(!job(kernel, child).stopped);
    expect_child_signal(parent, child, 6, 18);
    // Ignored CONT still continues: stop through the group, continue with SIG_IGN.
    thread.native_signal_mask = {};
    expect(linux_kill(kernel, parent, negative(child), 19) == 0 && job(kernel, child).stopped);
    expect_child_signal(parent, child, 5, 19);
    handle(kernel, child, 18, 1);
    expect(linux_kill(kernel, parent, child, 18) == 0 && !job(kernel, child).stopped);
    expect_child_signal(parent, child, 6, 18);
    // The continued thread is entered again.
    bool resumed = false;
    for (unsigned i = 0; i < 6 && !resumed; ++i)
    {
        mock::last_frame = {};
        static_cast<void>(native::step(kernel));
        resumed = mock::last_frame.registers[0] == marker;
    }
    expect(resumed && native::runtime_status().ok());
    // A caught signal waits in the queue of a stopped process; KILL still acts.
    handle(kernel, child, 10);
    expect(linux_kill(kernel, parent, child, 19) == 0 && job(kernel, child).stopped);
    expect(linux_kill(kernel, parent, child, 10) == 0 && shared(child).pending_mask().contains(10));
    // A terminating default sent to a stopped process stays pending until the
    // continue (Linux wants_signal); the process stays stopped.
    expect(linux_kill(kernel, parent, child, 15) == 0 && !native::private_signal_find(child)->deferred_fatal);
    expect(shared(child).pending_mask().contains(15) && job(kernel, child).stopped);
    native::private_process_signal_fatal(kernel);
    expect(job(kernel, child).stopped && kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
    expect(linux_call(kernel, parent, lu::Operation::wait4, {negative(child), status_at, 3, 0}) == static_cast<i64>(child));
    expect(status_word() == ((19U << 8) | 0x7f));
    expect(linux_kill(kernel, parent, child, 9) == 0 && native::private_signal_find(child)->group_kill);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(child)->state() == sched::ProcessState::zombie);
    expect(kernel.scheduler().find(child)->wait_status() == 9 && kernel.scheduler().stopped_processes() == 0);
    expect(linux_call(kernel, parent, lu::Operation::wait4, {negative(child), status_at, 1, 0}) == static_cast<i64>(child));
    expect(status_word() == 9 && !kernel.scheduler().find(child));
    drain(parent);
    release(kernel, parent);
}

// A terminating default held by a stopped process acts once it is continued.
void stopped_fatal(Kernel &kernel)
{
    const auto parent = create(kernel);
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    expect(linux_kill(kernel, parent, child, 19) == 0 && linux_kill(kernel, parent, child, 1) == 0);
    expect(job(kernel, child).stopped && shared(child).pending_mask().contains(1));
    expect(linux_kill(kernel, parent, child, 18) == 0 && !job(kernel, child).stopped);
    auto &process = *kernel.scheduler().find(child);
    expect(!native::private_signal_deliver(kernel, process, *kernel.scheduler().find_thread(child, child)));
    expect(process.state() == sched::ProcessState::zombie && process.wait_status() == 1);
    drain(parent);
    release(kernel, child); release(kernel, parent);
}

// SA_NOCLDSTOP, SIG_DFL and blocked SIGCHLD; a native parent waits without a signal.
void notifications(Kernel &kernel)
{
    const auto parent = create(kernel);
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    action(kernel, parent, handler, Codec::no_child_stop);
    expect(linux_kill(kernel, parent, child, 19) == 0 && job(kernel, child).stopped);
    expect(linux_kill(kernel, parent, child, 18) == 0 && !job(kernel, child).stopped);
    expect(!shared(parent).pending_mask().contains(sigchld));
    action(kernel, parent, 0);
    expect(linux_kill(kernel, parent, child, 19) == 0 && !shared(parent).pending_mask().contains(sigchld));
    kernel.scheduler().find_thread(parent, parent)->native_signal_mask.set(sigchld);
    expect(linux_kill(kernel, parent, child, 18) == 0);
    expect_child_signal(parent, child, 6, 18);
    kernel.scheduler().find_thread(parent, parent)->native_signal_mask = {};
    // A native parent gets no signal; native wait4 reports with OK_W* options.
    kernel.scheduler().find(parent)->resources().personality = sched::UserPersonality::native;
    expect(native_kill(kernel, parent, child, OK_SIGTSTP) == 0 && job(kernel, child).stopped);
    expect(!shared(parent).pending_mask().contains(sigchld));
    expect(native_call(kernel, parent, N::wait4, {child, status_at, 0x10, 0}) == -22);
    expect(native_call(kernel, parent, N::wait4, {0x80000000U, status_at, OK_WNOHANG, 0}) == -3);
    expect(native_call(kernel, parent, N::wait4, {child, status_at, OK_WNOHANG, 0}) == 0);
    expect(native_call(kernel, parent, N::wait4, {child, status_at, OK_WNOHANG | OK_WUNTRACED, 0}) == static_cast<i64>(child));
    expect(status_word() == ((20U << 8) | 0x7f));
    expect(native_kill(kernel, parent, child, OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    expect(native_call(kernel, parent, N::wait4, {negative(child), status_at, OK_WNOHANG | OK_WCONTINUED, 0}) ==
           static_cast<i64>(child) && status_word() == 0xffffU);
    kernel.scheduler().find(parent)->resources().personality = sched::UserPersonality::linux;
    release(kernel, child); release(kernel, parent);
}

// Native recipients: every signal through kill and group kill, SIG_IGN through
// native rt_sigaction (inherited across fork), and its exact error order.
void native_dispositions(Kernel &kernel)
{
    const auto parent = create(kernel);
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    for (auto pid : {parent, child}) kernel.scheduler().find(pid)->resources().personality = sched::UserPersonality::native;
    const auto sigaction = [&](u64 pid, u64 signal, uptr act, uptr old, u64 size = 8) {
        return native_call(kernel, pid, N::rt_sigaction, {signal, act, old, size});
    };
    const auto set_act = [&](u64 handler_value, u64 flags = 0, u64 mask = 0) {
        put64(act_at, handler_value); put64(act_at + 8, flags); put64(act_at + 16, mask);
    };
    set_act(OK_SIG_IGN);
    expect(sigaction(child, OK_SIGTSTP, 1, 0, 4) == -22);            // sigsetsize first
    expect(sigaction(child, 0, hw::user_end - 8, 0) == -14);         // then the act copy
    expect(sigaction(child, 0, act_at, 0) == -22 && sigaction(child, 65, act_at, 0) == -22);
    expect(sigaction(child, OK_SIGKILL, act_at, 0) == -22 && sigaction(child, OK_SIGSTOP, act_at, 0) == -22);
    expect(sigaction(child, OK_SIGKILL, 0, old_at) == 0 && get64(old_at) == OK_SIG_DFL);
    set_act(2); expect(sigaction(child, OK_SIGTSTP, act_at, 0) == -22);
    set_act(OK_SIG_IGN, 1); expect(sigaction(child, OK_SIGTSTP, act_at, 0) == -22);
    set_act(OK_SIG_IGN, 0, 1); expect(sigaction(child, OK_SIGTSTP, act_at, 0) == -22);
    set_act(OK_SIG_IGN); expect(sigaction(child, OK_SIGCHLD, act_at, 0) == -22);
    set_act(OK_SIG_DFL); expect(sigaction(child, OK_SIGCHLD, act_at, 0) == 0);
    // Commit happens before the oact fault.
    set_act(OK_SIG_IGN);
    mock::write_forbidden = old_at;
    expect(sigaction(child, OK_SIGTSTP, act_at, old_at) == -14);
    mock::write_forbidden = 0;
    expect(native::private_signal_table(kernel, child)->find(OK_SIGTSTP)->handler == OK_SIG_IGN);
    put64(old_at + 8, 7);
    expect(sigaction(child, OK_SIGTSTP, 0, old_at) == 0 && get64(old_at) == OK_SIG_IGN && get64(old_at + 8) == 0);
    // An ignored stop does nothing; the uncatchable stop and others act.
    expect(native_kill(kernel, parent, child, OK_SIGTSTP) == 0 && !job(kernel, child).stopped);
    expect(native_kill(kernel, parent, negative(child), OK_SIGSTOP) == 0 && job(kernel, child).stopped);
    expect(native_call(kernel, parent, N::wait4, {child, status_at, OK_WNOHANG | OK_WUNTRACED, 0}) ==
           static_cast<i64>(child) && status_word() == ((19U << 8) | 0x7f));
    expect(native_kill(kernel, parent, negative(child), OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    // SIG_IGN is inherited across fork.
    activate(kernel, child, child);
    const auto grandchild = fork(kernel, child);
    expect(native::private_signal_table(kernel, grandchild)->find(OK_SIGTSTP)->handler == OK_SIG_IGN);
    release(kernel, grandchild);
    // A native process has no SIGCHLD disposition: a default-ignored signal does nothing.
    expect(native_kill(kernel, parent, child, OK_SIGWINCH) == 0 && !native::private_signal_find(child)->deferred_fatal);
    // A terminating default is decided at generation; the first fatal wins. A
    // stopped process applies it only after SIGCONT.
    expect(native_kill(kernel, parent, child, OK_SIGSTOP) == 0 && job(kernel, child).stopped);
    expect(native_kill(kernel, parent, child, OK_SIGINT) == 0 && native_kill(kernel, parent, child, OK_SIGHUP) == 0);
    expect(native::private_signal_find(child)->deferred_fatal == OK_SIGINT);
    native::private_process_signal_fatal(kernel);
    expect(job(kernel, child).stopped && kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
    expect(native_kill(kernel, parent, child, OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    // A process condemned by a fatal signal no longer stops.
    expect(native_kill(kernel, parent, child, OK_SIGSTOP) == 0 && !job(kernel, child).stopped);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(child)->wait_status() == OK_SIGINT);
    expect(native_call(kernel, parent, N::wait4, {child, status_at, 0, 0}) == static_cast<i64>(child) &&
           status_word() == OK_SIGINT);
    // tgkill of a native thread stays refused.
    kernel.scheduler().find(parent)->resources().personality = sched::UserPersonality::linux;
    const auto other = create(kernel);
    kernel.scheduler().find(other)->resources().personality = sched::UserPersonality::native;
    expect(linux_call(kernel, parent, lu::Operation::tkill, {other, 19}) == -95 && !job(kernel, other).stopped);
    // A Linux sender reaches a native recipient through the same core.
    expect(linux_kill(kernel, parent, other, 19) == 0 && job(kernel, other).stopped);
    expect(linux_kill(kernel, parent, other, 18) == 0 && !job(kernel, other).stopped);
    release(kernel, other); release(kernel, parent);
}

// Init (SIGNAL_UNKILLABLE) in Linux sig_ignored order: unblocked SIG_DFL
// terminating and stopping signals are dropped at generation from every
// sender, blocked ones are queued and dropped when dequeued.
void init_protection(Kernel &kernel, u64 init)
{
    const auto parent = create(kernel);
    expect(linux_kill(kernel, parent, init, 15) == 0 && linux_kill(kernel, parent, init, 19) == 0);
    expect(linux_kill(kernel, parent, init, 9) == 0 && linux_kill(kernel, parent, init, 20) == 0);
    const auto *state = native::private_signal_find(init);
    expect(!job(kernel, init).stopped && !state->group_kill && !state->deferred_fatal);
    expect(!state->shared.pending_mask().contains(15) && !state->shared.pending_mask().contains(20));
    expect(native_kill(kernel, parent, init, OK_SIGCONT) == -1);
    auto &process = *kernel.scheduler().find(init);
    auto &thread = *kernel.scheduler().find_thread(init, init);
    auto *queue = native::private_signal_queue(thread);
    // Blocked: queued (rt_sigpending sees it), then dropped by delivery.
    thread.native_signal_mask.set(15);
    expect(linux_kill(kernel, parent, init, 15) == 0 && state->shared.pending_mask().contains(15));
    thread.native_signal_mask = {};
    expect(native::private_signal_deliver(kernel, process, thread) && !state->shared.pending_mask().contains(15));
    // Queued while caught, delivered after a reset to SIG_DFL: dropped.
    handle(kernel, init, 10);
    expect(linux_kill(kernel, parent, init, 10) == 0 && state->shared.pending_mask().contains(10));
    handle(kernel, init, 10, 0);
    expect(native::private_signal_deliver(kernel, process, thread) && !state->shared.pending_mask().contains(10));
    // SIGSTOP is ignored but still discards a pending SIGCONT first.
    handle(kernel, init, 18); thread.native_signal_mask.set(18);
    expect(linux_kill(kernel, parent, init, 18) == 0 && state->shared.pending_mask().contains(18));
    expect(linux_kill(kernel, parent, init, 19) == 0 && !state->shared.pending_mask().contains(18));
    thread.native_signal_mask = {}; handle(kernel, init, 18, 0);
    // A wait is never completed by a signal init drops (Linux restarts the
    // call); a caught signal still ends it, and once unblocked the other is
    // dropped by delivery.
    thread.native_signal_mask.set(15);
    expect(linux_kill(kernel, parent, init, 15) == 0 && state->shared.pending_mask().contains(15));
    put64(act_at, 0);
    activate(kernel, init, init);
    const auto saved = thread.native_frame;
    thread.native_frame.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::rt_sigsuspend);
    thread.native_frame.registers[9] = act_at; thread.native_frame.registers[10] = 8;
    bool complete = true; i64 result = -999; thread.syscall_pending = true;
    expect(native::private_signal_call(kernel, process, thread, complete, result) && !complete && result == 0);
    expect(!native::private_signal_wait_ready(kernel, process, thread, 0));
    handle(kernel, init, 10);
    expect(linux_kill(kernel, parent, init, 10) == 0 && native::private_signal_wait_ready(kernel, process, thread, 0));
    static_cast<void>(native::private_signal_wait_complete(kernel, process, thread, 0));
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    thread.native_frame = saved; thread.native_signal_mask = {}; handle(kernel, init, 10, 0);
    expect(state->shared.pending_mask().contains(15) && native::private_signal_deliver(kernel, process, thread));
    expect(!state->shared.pending_mask().contains(15) && process.state() != sched::ProcessState::zombie);
    // Thread-directed timers: SIGTERM and SIGKILL at SIG_DFL are dropped.
    for (u32 signal : {15U, 9U})
    {
        const auto id = arm_timer(kernel, init, signal, static_cast<u32>(init));
        timer_service(kernel);
        expect(!queue->pending_mask().contains(signal) && process.state() != sched::ProcessState::zombie);
        expect(linux_call(kernel, init, lu::Operation::timer_delete, {id}) == 0);
    }
    // SIGPIPE at SIG_DFL: init only sees EPIPE.
    activate(kernel, init, init); thread.syscall_pending = true;
    native::queue_pipe_sigpipe(kernel, process, init);
    thread.syscall_pending = false;
    expect(native::runtime_status().ok() && !queue->pending_mask().contains(13));
    expect(native::private_signal_deliver(kernel, process, thread) && process.state() != sched::ProcessState::zombie);
    // A TERM the leader blocks while another thread, parked, does not: queued,
    // never the complete_signal fatal shortcut, and dropped once dequeued.
    const auto helper = peer(kernel, init);
    activate(kernel, init, helper);
    auto wait = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read, 0); CHECK(wait);
    auto &parked = *kernel.scheduler().find_thread(init, helper);
    parked.syscall_pending = true;
    thread.native_signal_mask.set(15);
    expect(linux_kill(kernel, parent, init, 15) == 0 && !state->deferred_fatal && state->shared.pending_mask().contains(15));
    thread.native_signal_mask = {};
    expect(native::private_signal_deliver(kernel, process, thread) && !state->shared.pending_mask().contains(15));
    CHECK(kernel.scheduler().complete_wait(wait.value(), sched::WaitCompletion::ready));
    parked.syscall_pending = false;
    if (parked.state == sched::ProcessState::blocked) parked.state = sched::ProcessState::runnable;
    release(kernel, parent);
}

// Same-session SIGCONT and the orphaned-group TSTP drop.
void protections(Kernel &kernel)
{
    const auto parent = create(kernel);
    // SIGCONT crosses credentials inside the session only.
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    auto credentials = kernel.scheduler().find(child)->credentials();
    credentials.uid = credentials.euid = credentials.suid = 4321;
    CHECK(kernel.scheduler().set_credentials(child, credentials));
    expect(linux_kill(kernel, parent, child, 19) == 0 && job(kernel, child).stopped);
    credentials.uid = credentials.euid = credentials.suid = 1234;
    CHECK(kernel.scheduler().set_credentials(parent, credentials));
    expect(linux_kill(kernel, parent, child, 15) == -1 && linux_kill(kernel, parent, child, 18) == 0);
    expect(!job(kernel, child).stopped);
    // The same exception for native kill and tkill (send_directed_signal).
    const auto restop = [&] {
        auto root = credentials; root.uid = root.euid = root.suid = 0;
        CHECK(kernel.scheduler().set_credentials(parent, root));
        expect(linux_kill(kernel, parent, child, 19) == 0 && job(kernel, child).stopped);
        CHECK(kernel.scheduler().set_credentials(parent, credentials));
    };
    restop();
    expect(native_kill(kernel, parent, child, OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    restop();
    expect(linux_call(kernel, parent, lu::Operation::tkill, {child, 18}) == 0 && !job(kernel, child).stopped);
    expect(linux_call(kernel, parent, lu::Operation::tkill, {child, 10}) == -1);
    drain(parent);
    expect(native_kill(kernel, parent, child, OK_SIGHUP) == -1);
    expect(linux_kill(kernel, parent, child, 19) == -1);
    const auto stranger = create(kernel);
    CHECK(kernel.scheduler().set_credentials(stranger, credentials));
    expect(linux_kill(kernel, stranger, child, 18) == -1 && native_kill(kernel, stranger, child, OK_SIGCONT) == -1);
    credentials.uid = credentials.euid = credentials.suid = 0;
    CHECK(kernel.scheduler().set_credentials(parent, credentials));
    // A process with no parent is an orphaned group: TSTP is dropped, STOP is not.
    expect(kernel.scheduler().process_group_orphaned(stranger) && native::private_group_orphaned(kernel, stranger));
    expect(!native::private_group_orphaned(kernel, child));
    expect(linux_kill(kernel, parent, stranger, 20) == 0 && !job(kernel, stranger).stopped);
    expect(native_kill(kernel, parent, stranger, OK_SIGTTIN) == 0 && !job(kernel, stranger).stopped);
    expect(!shared(stranger).pending_mask().contains(20));
    // Blocked, it stays pending (Linux drops it only at dequeue), then is dropped.
    auto &lone = *kernel.scheduler().find_thread(stranger, stranger);
    lone.native_signal_mask.set(20);
    expect(linux_kill(kernel, parent, stranger, 20) == 0 && shared(stranger).pending_mask().contains(20));
    lone.native_signal_mask = {};
    expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(stranger), lone));
    expect(!job(kernel, stranger).stopped && !shared(stranger).pending_mask().contains(20));
    expect(linux_kill(kernel, parent, stranger, 19) == 0 && job(kernel, stranger).stopped);
    expect(linux_kill(kernel, parent, stranger, 18) == 0 && !job(kernel, stranger).stopped);
    // The TTY layer's ignored-or-blocked question takes native numbers.
    auto &thread = *kernel.scheduler().find_thread(child, child);
    expect(!native::private_signal_ignored_or_blocked(kernel, *kernel.scheduler().find(child), thread, OK_SIGTTOU));
    thread.native_signal_mask.set(22);
    expect(native::private_signal_ignored_or_blocked(kernel, *kernel.scheduler().find(child), thread, OK_SIGTTOU));
    thread.native_signal_mask = {};
    handle(kernel, child, 21, 1);
    expect(native::private_signal_ignored_or_blocked(kernel, *kernel.scheduler().find(child), thread, OK_SIGTTIN));
    release(kernel, stranger); release(kernel, child); release(kernel, parent);
}

// Exit of the process that held a stopped group: SIGHUP then SIGCONT.
void orphan_pass(Kernel &kernel, bool caught)
{
    const auto leader = create(kernel);
    const auto member = fork(kernel, leader);
    grouped(kernel, leader, member);
    if (caught) handle(kernel, member, 1);
    expect(!kernel.scheduler().process_group_orphaned(member));
    expect(linux_kill(kernel, leader, member, 19) == 0 && job(kernel, member).stopped);
    raw_exit(kernel, leader, leader, 0, true);
    expect(kernel.scheduler().find(member)->parent_pid() == sched::user_init_pid);
    expect(!job(kernel, member).stopped && job(kernel, member).continue_report);
    // HUP reached the stopped member first, so it waits in the queue (caught
    // or not) and acts after the continue.
    expect(shared(member).pending_mask().contains(1) && !native::private_signal_find(member)->deferred_fatal);
    if (!caught)
    {
        auto &process = *kernel.scheduler().find(member);
        expect(!native::private_signal_deliver(kernel, process, *kernel.scheduler().find_thread(member, member)));
        expect(process.wait_status() == 1);
    }
    release(kernel, member);
    CHECK(kernel.scheduler().reap_process(leader));
}

// Linux kill_orphaned_pgrp for the exiting process's own group: P leaves its
// group G, whose other member is P's stopped child S. G is orphaned when P's
// parent Q sits in another group of the session; a sibling S keeps it held.
void own_group_orphan(Kernel &kernel, bool child_member)
{
    const auto q = create(kernel);
    const auto p = fork(kernel, q);
    const auto s = fork(kernel, child_member ? p : q);
    CHECK(kernel.scheduler().set_process_group(p, 0, 0));
    CHECK(kernel.scheduler().set_process_group(child_member ? p : q, static_cast<i32>(s), static_cast<i32>(p)));
    handle(kernel, s, 1);
    expect(!kernel.scheduler().process_group_orphaned(p));
    expect(linux_kill(kernel, p, s, 19) == 0 && job(kernel, s).stopped);
    raw_exit(kernel, p, p, 0, true);
    if (child_member)
    {
        expect(kernel.scheduler().find(s)->parent_pid() == sched::user_init_pid);
        expect(!job(kernel, s).stopped && job(kernel, s).continue_report && shared(s).pending_mask().contains(1));
    }
    else expect(job(kernel, s).stopped && !shared(s).pending_mask().contains(1));
    CHECK(kernel.scheduler().reap_process(p));
    drain(q);
    release(kernel, s); release(kernel, q);
}

// Kernel senders: a caught signal no thread can take is owed and drained once
// a wait completes; SIG_IGN, CONT, pid-slot reuse and an exec into another
// personality drop it, a same-personality exec keeps it.
void owed(Kernel &kernel)
{
    const auto pid = create(kernel);
    handle(kernel, pid, 1); handle(kernel, pid, 20); handle(kernel, pid, 2);
    // Several owed signals all drain in one pass.
    auto ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGINT) == native::KernelSignalSend::owed);
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel));
    expect(shared(pid).pending_mask().contains(1) && shared(pid).pending_mask().contains(2));
    drain(pid);
    expect(!native::private_jobctl_service(kernel) && !shared(pid).pending_mask().contains(1));
    // A signal still without a taker stays owed while the others drain.
    const auto helper = peer(kernel, pid);
    auto &other = *kernel.scheduler().find_thread(pid, helper);
    other.native_signal_mask.set(1); other.native_signal_mask.set(2);
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGINT) == native::KernelSignalSend::owed);
    other.native_signal_mask = {}; other.native_signal_mask.set(2);
    expect(!native::private_jobctl_service(kernel));
    expect(shared(pid).pending_mask().contains(1) && !shared(pid).pending_mask().contains(2));
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel) && shared(pid).pending_mask().contains(2));
    drain(pid);
    // The helper blocks every signal used below from here on.
    other.native_signal_mask.set(1); other.native_signal_mask.set(20);
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    // A caught stop acted at generation, so it is queued rather than owed.
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGTSTP) == native::KernelSignalSend::sent);
    expect(!shared(pid).pending_mask().contains(1) && shared(pid).pending_mask().contains(20));
    expect(!native::private_jobctl_service(kernel) && !shared(pid).pending_mask().contains(1));
    // CONT discards the queued TSTP; SIGCONT itself is ignored by default.
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGCONT) == native::KernelSignalSend::ignored);
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel));
    native::PrivateProcessQueue::Delivery delivered;
    expect(shared(pid).peek({}, delivered) && delivered.info == sched::PendingSignalInfo{1, 128, 0, 0});
    expect(!shared(pid).pending_mask().contains(20));
    drain(pid);
    // SIG_IGN through rt_sigaction drops an owed signal.
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    sched::SignalAction ignored{}; ignored.handler = 1;
    const auto bytes = Codec::encode(ignored);
    std::memcpy(mock::memory.data() + act_at - hw::user_begin, bytes.data(), bytes.size());
    unpark(kernel, pid, ticket);
    expect(linux_call(kernel, pid, lu::Operation::rt_sigaction, {1, act_at, 0, 8}) == 0);
    handle(kernel, pid, 1);
    expect(!native::private_jobctl_service(kernel) && !shared(pid).pending_mask().contains(1));
    // A new signal epoch (the pid slot reused) drops it as well.
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    ++native::private_signal_find(pid)->epoch;
    static_cast<void>(native::private_jobctl_service(kernel));
    --native::private_signal_find(pid)->epoch;
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel) && !shared(pid).pending_mask().contains(1));
    // The exec hook: a same-personality exec keeps it (Linux shared pending)...
    auto &process = *kernel.scheduler().find(pid);
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    native::private_jobctl_exec(kernel, process, sched::UserPersonality::linux);
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel) && shared(pid).pending_mask().contains(1));
    drain(pid);
    // ...and an exec into the native personality drops it, and resets an
    // inherited SIGCHLD SIG_IGN that the native contract refuses.
    ticket = park(kernel, pid);
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    handle(kernel, pid, sigchld, 1); handle(kernel, pid, 2, 1);
    process.resources().personality = sched::UserPersonality::native;
    native::private_jobctl_exec(kernel, process, sched::UserPersonality::linux);
    unpark(kernel, pid, ticket);
    expect(native_call(kernel, pid, N::rt_sigaction, {OK_SIGCHLD, 0, old_at, 8}) == 0 && get64(old_at) == OK_SIG_DFL);
    expect(native_call(kernel, pid, N::rt_sigaction, {OK_SIGINT, 0, old_at, 8}) == 0 && get64(old_at) == OK_SIG_IGN);
    process.resources().personality = sched::UserPersonality::linux;
    handle(kernel, pid, 2, 0);
    expect(!native::private_jobctl_service(kernel) && !shared(pid).pending_mask().contains(1));
    // A user kill of the same signal is refused instead (EOPNOTSUPP).
    ticket = park(kernel, pid);
    const auto sender = create(kernel);
    expect(linux_kill(kernel, sender, pid, 1) == -95);
    release(kernel, sender);
    // Group form: counts per member.
    const auto group = native::send_kernel_group_signal(kernel, pid, OK_SIGHUP);
    expect(group.matched == 1 && group.owed == 1 && !group.sent);
    expect(native::send_kernel_group_signal(kernel, pid, OK_SIGWINCH).ignored == 1);
    unpark(kernel, pid, ticket);
    expect(!native::private_jobctl_service(kernel) && shared(pid).pending_mask().contains(1));
    drain(pid);
    expect(native::send_kernel_process_signal(kernel, 0x7ffffff0, OK_SIGHUP) == native::KernelSignalSend::departed);
    release(kernel, pid);
}

// A caught CONT or stop has acted at generation (discards, continue), so a
// user sender gets 0 and the signal is queued, even when no thread can take
// it now; other caught signals still refuse.
void acted_not_refused(Kernel &kernel)
{
    const auto pid = create(kernel), sender = create(kernel);
    handle(kernel, pid, 1); handle(kernel, pid, 18); handle(kernel, pid, 20);
    const auto ticket = park(kernel, pid);
    expect(linux_kill(kernel, sender, pid, 19) == 0 && job(kernel, pid).stopped);
    expect(linux_kill(kernel, sender, pid, 18) == 0 && !job(kernel, pid).stopped && job(kernel, pid).continue_report);
    expect(shared(pid).pending_mask().contains(18));
    expect(linux_kill(kernel, sender, pid, 20) == 0 && shared(pid).pending_mask().contains(20));
    expect(!shared(pid).pending_mask().contains(18));
    expect(linux_call(kernel, sender, lu::Operation::tkill, {pid, 18}) == 0);
    expect(!shared(pid).pending_mask().contains(20) && native::private_signal_queue(
        *kernel.scheduler().find_thread(pid, pid))->pending_mask().contains(18));
    expect(linux_kill(kernel, sender, negative(pid), 18) == 0 && shared(pid).pending_mask().contains(18));
    expect(linux_kill(kernel, sender, pid, 1) == -95);
    unpark(kernel, pid, ticket);
    release(kernel, sender); release(kernel, pid);
}

// A timer retry neither repeats its CONT side effects nor survives the
// opposite stop, which flushes the record Linux queued at expiry.
void timer_retry(Kernel &kernel)
{
    const auto pid = create(kernel), sender = create(kernel);
    handle(kernel, pid, 18);
    const auto id = arm_timer(kernel, pid, 18);
    const auto ticket = park(kernel, pid);
    expect(linux_kill(kernel, sender, pid, 19) == 0 && linux_kill(kernel, sender, pid, 18) == 0);
    expect(!job(kernel, pid).stopped && shared(pid).pending_mask().contains(18));
    // The expiry coalesces with the pending CONT and stays owed to its timer.
    timer_service(kernel);
    expect(native::private_timer_counters().retried > 0);
    expect(linux_kill(kernel, sender, pid, 19) == 0 && job(kernel, pid).stopped);
    expect(!shared(pid).pending_mask().contains(18));
    for (unsigned i = 0; i < 3; ++i) timer_service(kernel);
    expect(job(kernel, pid).stopped && !shared(pid).pending_mask().contains(18));
    expect(linux_kill(kernel, sender, pid, 18) == 0);
    unpark(kernel, pid, ticket);
    drain(pid);
    expect(linux_call(kernel, pid, lu::Operation::timer_delete, {id}) == 0);
    release(kernel, sender); release(kernel, pid);
}

// A fatal default the owed drain decides in a step ends the process in that
// step: its runnable thread is never entered again.
void same_step_fatal(Kernel &kernel)
{
    const auto parent = create(kernel), pid = fork(kernel, parent);
    handle(kernel, pid, 1);
    const auto helper = peer(kernel, pid);
    kernel.scheduler().find_thread(pid, helper)->native_signal_mask.set(1);
    static_cast<void>(park(kernel, pid));
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGHUP) == native::KernelSignalSend::owed);
    handle(kernel, pid, 1, 0);
    native::started = true;
    static_cast<void>(native::step(kernel));
    expect(native::runtime_status().ok());
    expect(kernel.scheduler().find(pid)->state() == sched::ProcessState::zombie);
    expect(kernel.scheduler().find(pid)->wait_status() == 1);
    release(kernel, pid);
    drain(parent);
    release(kernel, parent);
}

// Shutdown's terminate path completes a stopped process.
void shutdown_stopped(Kernel &kernel)
{
    const auto parent = create(kernel);
    const auto child = fork(kernel, parent);
    grouped(kernel, parent, child);
    expect(linux_kill(kernel, parent, child, 19) == 0 && kernel.scheduler().stopped_processes() == 1);
    CHECK(kernel.posix().activate(0));
    expect(native::terminate_native_process(kernel, child, 9).ok());
    expect(kernel.scheduler().find(child)->state() == sched::ProcessState::zombie);
    expect(kernel.scheduler().stopped_processes() == 0 && job(kernel, child) == sched::JobControlState{});
    drain(parent);
    release(kernel, child); release(kernel, parent);
}
} // namespace

int main()
{
    static_assert(!native::native_signal_diagnostics);
    numbering<lu::Profile::i386>(); numbering<lu::Profile::x86_64>(); numbering<lu::Profile::aarch64>();
    numbering<lu::Profile::arm32>(); numbering<lu::Profile::rv64>(); numbering<lu::Profile::rv32>();
    numbering<lu::Profile::loongarch64>(); numbering<lu::Profile::mips>(); numbering<lu::Profile::mips64>();
    numbering<lu::Profile::ppc>();
    static Kernel kernel; CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto init = create_init(kernel);
    linux_stop_continue(kernel);
    stopped_fatal(kernel);
    notifications(kernel);
    native_dispositions(kernel);
    init_protection(kernel, init);
    protections(kernel);
    orphan_pass(kernel, true);
    orphan_pass(kernel, false);
    own_group_orphan(kernel, true);
    own_group_orphan(kernel, false);
    owed(kernel);
    acted_not_refused(kernel);
    timer_retry(kernel);
    same_step_fatal(kernel);
    shutdown_stopped(kernel);
    expect(native::runtime_status().ok() && kernel.scheduler().stopped_processes() == 0);
    expect(kernel.scheduler().find(init)->state() != sched::ProcessState::zombie);
    std::printf("NATIVE_JOB_CONTROL_RUNTIME_PASS native_calls=%zu linux_calls=%zu profiles=%zu hosted=1 guest=0\n",
                native_calls, linux_calls, profiles);
}
