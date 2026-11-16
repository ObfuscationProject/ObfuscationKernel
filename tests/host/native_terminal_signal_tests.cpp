// Linux terminal waits in the actual runtime: PTY reads and writes of the
// Linux personality on all ten profiles, Linux ppoll of PTY endpoints, caught
// signals interrupting blocked PTY and console reads and PTY writes (EINTR,
// SA_RESTART through the captured syscall origin, the committed count of a
// partial write), the kill admission of such a wait, terminating defaults,
// readiness (the call moves a byte now) winning over a pending signal, signals
// that do not interrupt, PTY VTIME timers through the actual step(), and the
// job-control park of a background member (EINTR, and an SA_RESTART restart of
// TCSETS). Hosted trap, copy and mapping backends; no guest execution.
#define OK_NATIVE_MODEL_MEMORY_BYTES 32768
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#define OK_ENABLE_TEST_POINTS 1
#include "native_child_exit_runtime_fixture.hpp"

namespace
{
using N = syscall::Number;
using Wire = tty::NativeTermiosWire<false>;
constexpr uptr pair_at = hw::user_begin + 0x2400, poll_at = hw::user_begin + 0x2480, time_at = hw::user_begin + 0x24c0,
               argument_at = hw::user_begin + 0x2500, vectors_at = hw::user_begin + 0x2600,
               read_at = hw::user_begin + 0x2800, input_at = hw::user_begin + 0x4000;
constexpr u64 sa_restart = 0x10000000;
constexpr u64 linux_read = 0, linux_write = 1, linux_ioctl = 16, linux_readv = 19;
usize linux_calls{};

std::byte *user(uptr address) { return mock::memory.data() + (address - hw::user_begin); }
sched::ThreadControlBlock &thread_of(Kernel &kernel, u64 pid, u64 tid) { return *kernel.scheduler().find_thread(pid, tid); }

// A raw pair made while the caller is native (OK_SYS_PTY_PAIR stays native-only).
std::array<i32, 2> make_pair(Kernel &kernel, u64 pid)
{
    auto &process = *kernel.scheduler().find(pid);
    const auto personality = process.resources().personality;
    process.resources().personality = sched::UserPersonality::native;
    activate(kernel, pid, pid);
    bool complete = true;
    expect(native::dispatch(kernel, pid, {OK_SYS_PTY_PAIR, {pair_at, 0, 0, 0, 0, 0}}, complete) == 0 && complete);
    process.resources().personality = personality;
    std::array<i32, 2> fds{};
    std::memcpy(fds.data(), user(pair_at), sizeof(fds));
    return fds;
}
// A native ioctl by the leader (another thread of the process may wait).
i64 native_ioctl(Kernel &kernel, u64 pid, i32 fd, u64 request, uptr argument)
{
    auto &process = *kernel.scheduler().find(pid);
    const auto personality = process.resources().personality;
    process.resources().personality = sched::UserPersonality::native;
    activate(kernel, pid, pid);
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {OK_SYS_IOCTL, {static_cast<uptr>(fd), static_cast<uptr>(request), argument, 0, 0, 0}}, complete);
    expect(complete);
    process.resources().personality = personality;
    return result;
}
i64 set_termios(Kernel &kernel, u64 pid, i32 fd, const tty::Termios &termios)
{
    std::array<u8, 36> bytes{};
    expect(Wire::encode(termios, {}, bytes).ok());
    std::memcpy(user(argument_at), bytes.data(), bytes.size());
    return native_ioctl(kernel, pid, fd, OK_TCSETS, argument_at);
}
sched::Terminal &slave_terminal(Kernel &kernel, u64 pid, i32 fd)
{
    const auto *entry = kernel.scheduler().find(pid)->fd_table().entry(fd);
    const auto *description = kernel.scheduler().open_description(entry->description);
    return kernel.scheduler().terminal_storage(1 + description->pty_endpoint.slot);
}
void put_master(Kernel &kernel, u64 pid, i32 fd, std::string_view text)
{
    activate(kernel, pid, pid);
    const auto wrote = kernel.posix().write(fd, {reinterpret_cast<const std::byte *>(text.data()), text.size()});
    expect(wrote && wrote.value() == text.size());
}
usize take(Kernel &kernel, u64 pid, i32 fd, usize count)
{
    activate(kernel, pid, pid);
    std::array<std::byte, sched::pipe_capacity> bytes{};
    const auto read = kernel.posix().read(fd, {bytes.data(), count});
    return read ? read.value() : 0;
}

// The Linux x86_64 syscall instruction at `site`, entered through step().
void enter(Kernel &kernel, u64 pid, u64 tid, u64 number, u64 a0, u64 a1, u64 a2)
{
    auto &thread = thread_of(kernel, pid, tid);
    frame(thread);
    thread.native_frame.pc = site + 2;
    thread.native_frame.registers[14] = number;
    thread.native_frame.registers[9] = a0; thread.native_frame.registers[10] = a1; thread.native_frame.registers[11] = a2;
    *user(site) = std::byte{0x0f}; *user(site + 1) = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256};
    native::last_pid = pid; native::last_tid = tid - 1;
    expect(native::step(kernel));
    ++linux_calls;
}
bool pending(Kernel &kernel, u64 pid, u64 tid) { return thread_of(kernel, pid, tid).syscall_pending; }
i64 result_of(Kernel &kernel, u64 pid, u64 tid) { return static_cast<i64>(thread_of(kernel, pid, tid).native_frame.registers[14]); }
// One step that resumes the chosen thread (its readiness or timer completed).
void resume(Kernel &kernel, u64 pid, u64 tid)
{
    mock::entry_trap = {hw::TrapKind::timer};
    native::last_pid = pid; native::last_tid = tid - 1;
    static_cast<void>(native::step(kernel));
}
void caught(Kernel &kernel, u64 pid, u32 signal, bool restart)
{
    auto *entry = native::private_signal_table(kernel, pid)->find(signal); CHECK(entry);
    *entry = {}; entry->handler = handler; entry->restorer = handler + 4;
    entry->flags = 0x04000004 | (restart ? sa_restart : 0);
}
void enqueue(Kernel &kernel, u64 pid, u64 tid, u32 signal)
{
    auto *queue = native::private_signal_queue(thread_of(kernel, pid, tid)); CHECK(queue);
    expect(queue->enqueue({signal, -6, static_cast<u32>(pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}
i64 linux_kill(Kernel &kernel, u64 sender, u64 target, u64 signal)
{
    activate(kernel, sender, sender);
    auto &thread = thread_of(kernel, sender, sender);
    thread.native_frame.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::kill);
    thread.native_frame.registers[9] = target; thread.native_frame.registers[10] = signal;
    bool complete = true; i64 result = -999;
    expect(native::private_signal_call(kernel, *kernel.scheduler().find(sender), thread, complete, result) && complete);
    return result;
}
u64 linux_process(Kernel &kernel, u64 &reader)
{
    const auto pid = create(kernel);
    reader = peer(kernel, pid);
    // Only the peer runs: the leader stays out of user mode.
    thread_of(kernel, pid, pid).native_active = false;
    return pid;
}

// EINTR and SA_RESTART for a PTY read, through the family and through step().
void pty_read_signal(Kernel &kernel, u64 sender, bool restart)
{
    u64 reader{}; const auto pid = linux_process(kernel, reader);
    const auto fds = make_pair(kernel, pid);
    caught(kernel, pid, 10, restart);
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    auto &thread = thread_of(kernel, pid, reader);
    auto &process = *kernel.scheduler().find(pid);
    expect(pending(kernel, pid, reader) && thread.wait_reason == sched::ThreadWaitReason::pty_read);
    const auto object = native::pty_wait_object(pid, reader);
    const auto armed = *native::pty_wait_objects.resolve(object).value();
    // A wake that finds no data rearms the continuation and carries its binding.
    CHECK(kernel.scheduler().complete_wait(armed.ticket, sched::WaitCompletion::ready));
    resume(kernel, pid, reader);
    const auto wait = *native::pty_wait_objects.resolve(object).value();
    expect(pending(kernel, pid, reader) && wait.ticket.generation > armed.ticket.generation && wait.request == armed.request);
    const auto origin = native::syscall_origins.identity(process, thread);
    expect(wait.personality == sched::UserPersonality::linux && wait.origin == origin.capture && origin.capture);
    expect(native::syscall_origins.validate_bound(origin, thread.native_frame, native::pty_read_origin_binding(object, wait)));
    // A caught signal is admitted for a bound terminal wait (no EOPNOTSUPP).
    expect(native::private_signal_admit_blocked(kernel, process, thread));
    expect(!native::private_terminal_signal_ready(kernel, process, thread));
    expect(linux_kill(kernel, sender, pid, 10) == 0);
    expect(native::private_terminal_signal_ready(kernel, process, thread));
    const auto frame = thread.native_frame;
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && !native::pty_wait_object(pid, reader).pool && !native::syscall_origins.live(origin));
    expect(thread.native_frame.pc == (restart ? site : site + 2));
    expect(thread.native_frame.registers[14] == (restart ? linux_read : static_cast<u64>(-4)));
    for (usize index : {9, 10, 11}) expect(thread.native_frame.registers[index] == frame.registers[index]);
    expect(!native::private_terminal_signal_complete(kernel, process, thread));
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    if (restart)
    {
        // The restarted read waits again and completes with real data.
        enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
        const auto next = *native::pty_wait_objects.resolve(native::pty_wait_object(pid, reader)).value();
        expect(pending(kernel, pid, reader) && next.request > wait.request);
        put_master(kernel, pid, fds[0], "again");
        resume(kernel, pid, reader);
        expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 5 && *user(read_at) == std::byte{'a'});
    }
    release(kernel, pid);
}

// Through the actual step(): candidate selection, the completion chain and the
// handler entry; a kernel-originated SIGINT is sent, never owed.
void pty_read_step(Kernel &kernel)
{
    u64 reader{}; const auto pid = linux_process(kernel, reader);
    const auto fds = make_pair(kernel, pid);
    caught(kernel, pid, OK_SIGINT, false);
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    expect(pending(kernel, pid, reader));
    expect(native::send_kernel_process_signal(kernel, pid, OK_SIGINT) == native::KernelSignalSend::sent);
    resume(kernel, pid, reader);
    const auto &thread = thread_of(kernel, pid, reader);
    expect(!thread.syscall_pending && !native::pty_wait_object(pid, reader).pool && thread.native_frame.pc == handler);
    release(kernel, pid);
}

// Data present wins over a pending signal; a terminating default ends the process.
void pty_ready_and_default(Kernel &kernel)
{
    u64 reader{}; auto pid = linux_process(kernel, reader);
    auto fds = make_pair(kernel, pid);
    caught(kernel, pid, 10, false);
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    put_master(kernel, pid, fds[0], "hi");
    enqueue(kernel, pid, reader, 10);
    auto &process = *kernel.scheduler().find(pid);
    expect(native::private_terminal_signal_owned(kernel, process, thread_of(kernel, pid, reader)));
    expect(!native::private_terminal_signal_ready(kernel, process, thread_of(kernel, pid, reader)));
    resume(kernel, pid, reader);
    // The read completed first; its handler runs at the next entry.
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 2 && *user(read_at) == std::byte{'h'});
    resume(kernel, pid, reader);
    expect(thread_of(kernel, pid, reader).native_frame.pc == handler);
    release(kernel, pid);

    pid = linux_process(kernel, reader);
    fds = make_pair(kernel, pid);
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    enqueue(kernel, pid, reader, 15);
    expect(!native::private_terminal_signal_complete(kernel, *kernel.scheduler().find(pid), thread_of(kernel, pid, reader)));
    const auto state = kernel.scheduler().find(pid)->state();
    expect(state == sched::ProcessState::zombie || state == sched::ProcessState::exited);
    expect(!native::pty_wait_object(pid, reader).pool);
    release(kernel, pid);
}

// A write that made progress returns its committed count, even under SA_RESTART;
// one without progress gets EINTR or restarts.
void pty_write_signal(Kernel &kernel)
{
    u64 writer{}; auto pid = linux_process(kernel, writer);
    auto fds = make_pair(kernel, pid);
    caught(kernel, pid, 10, true);
    for (usize index = 0; index < 6000; ++index) *user(input_at + index) = static_cast<std::byte>('a' + index % 26);
    enter(kernel, pid, writer, linux_write, fds[0], input_at, 6000);
    expect(pending(kernel, pid, writer));
    auto &thread = thread_of(kernel, pid, writer);
    auto &process = *kernel.scheduler().find(pid);
    const auto object = native::pty_wait_object(pid, writer);
    const auto wait = *native::pty_wait_objects.resolve(object).value();
    expect(wait.writing && wait.committed == sched::pipe_capacity && wait.origin);
    const auto origin = native::syscall_origins.identity(process, thread);
    expect(native::syscall_origins.validate_bound(origin, thread.native_frame, native::pty_write_origin_binding(object, wait)));
    expect(!native::syscall_origins.prepare_restart(origin, thread.native_frame, native::pty_write_origin_binding(object, wait)));
    enqueue(kernel, pid, writer, 10);
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && thread.native_frame.pc == site + 2 &&
           thread.native_frame.registers[14] == sched::pipe_capacity);
    expect(take(kernel, pid, fds[1], sched::pipe_capacity) == sched::pipe_capacity);
    release(kernel, pid);

    // The slave's buffer is full before the call: no progress, so SA_RESTART restarts.
    pid = linux_process(kernel, writer);
    fds = make_pair(kernel, pid);
    caught(kernel, pid, 10, true);
    std::array<std::byte, sched::pipe_capacity> full{};
    activate(kernel, pid, pid);
    expect(kernel.posix().write(fds[0], full).value() == sched::pipe_capacity);
    enter(kernel, pid, writer, linux_write, fds[0], input_at, 3);
    expect(pending(kernel, pid, writer) && !native::pty_wait_objects.resolve(native::pty_wait_object(pid, writer)).value()->committed);
    enqueue(kernel, pid, writer, 10);
    auto &blocked = thread_of(kernel, pid, writer);
    expect(native::private_terminal_signal_complete(kernel, *kernel.scheduler().find(pid), blocked));
    expect(blocked.native_frame.pc == site && blocked.native_frame.registers[14] == linux_write);
    release(kernel, pid);
}

// Readiness of a blocked write is "it moves a byte now", not poll POLLOUT: a
// slave write whose CR LF does not fit the master queue (room 1) gets EINTR or
// a restart from a caught signal and is ended by a default SIGTERM (kill
// admission, then step()); poll still reports the room.
void pty_write_expansion(Kernel &kernel, u64 sender)
{
    for (const int mode : {0, 1, 2}) // EINTR, SA_RESTART, SIG_DFL SIGTERM
    {
        u64 writer{}; const auto pid = linux_process(kernel, writer);
        const auto fds = make_pair(kernel, pid);
        auto &terminal = slave_terminal(kernel, pid, fds[1]);
        terminal.termios.oflag |= tty::oflag::opost | tty::oflag::onlcr;
        std::array<std::byte, sched::pipe_capacity - 1> fill{}; fill.fill(std::byte{'f'});
        activate(kernel, pid, pid);
        expect(kernel.posix().write(fds[1], fill).value() == fill.size());
        *user(input_at) = std::byte{'\n'};
        enter(kernel, pid, writer, linux_write, fds[1], input_at, 1);
        auto &thread = thread_of(kernel, pid, writer);
        auto &process = *kernel.scheduler().find(pid);
        expect(pending(kernel, pid, writer) && thread.wait_reason == sched::ThreadWaitReason::pty_write);
        const auto *entry = process.fd_table().entry(fds[1]);
        expect(kernel.scheduler().pty_description_readiness(entry->description).value().writable);
        if (mode == 2)
        {
            expect(linux_kill(kernel, sender, pid, 15) == 0);
            resume(kernel, pid, writer);
            const auto state = kernel.scheduler().find(pid)->state();
            expect(state == sched::ProcessState::zombie || state == sched::ProcessState::exited);
            expect(!native::pty_wait_object(pid, writer).pool);
            release(kernel, pid);
            continue;
        }
        caught(kernel, pid, 10, mode == 1);
        enqueue(kernel, pid, writer, 10);
        expect(native::private_terminal_signal_ready(kernel, process, thread));
        expect(native::private_terminal_signal_complete(kernel, process, thread));
        expect(!thread.syscall_pending && !native::pty_wait_object(pid, writer).pool);
        expect(thread.native_frame.pc == (mode == 1 ? site : site + 2));
        expect(thread.native_frame.registers[14] == (mode == 1 ? linux_write : static_cast<u64>(-4)));
        expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
        release(kernel, pid);
    }
}

// A master write of ^C with input room but a full signal outbox moves nothing,
// so a caught signal interrupts it; with outbox room readiness wins.
void pty_master_signal_character(Kernel &kernel)
{
    u64 writer{}; const auto pid = linux_process(kernel, writer);
    const auto fds = make_pair(kernel, pid);
    auto &terminal = slave_terminal(kernel, pid, fds[1]);
    terminal.termios.lflag |= tty::lflag::isig; terminal.termios[tty::Cc::intr] = 3;
    std::array<std::byte, sched::pipe_capacity> full{}; full.fill(std::byte{'f'});
    activate(kernel, pid, pid);
    expect(kernel.posix().write(fds[0], full).value() == full.size());
    caught(kernel, pid, 10, false);
    *user(input_at) = std::byte{3};
    enter(kernel, pid, writer, linux_write, fds[0], input_at, 1);
    expect(pending(kernel, pid, writer));
    expect(take(kernel, pid, fds[1], 16) == 16);
    auto &thread = thread_of(kernel, pid, writer);
    auto &process = *kernel.scheduler().find(pid);
    enqueue(kernel, pid, writer, 10);
    expect(!native::private_terminal_signal_ready(kernel, process, thread));
    for (usize index = 0; index < sched::terminal_outbox_capacity; ++index)
        expect(terminal.owe(1000 + index, tty::TerminalSignal::interrupt, true));
    expect(native::private_terminal_signal_ready(kernel, process, thread));
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && thread.native_frame.registers[14] == static_cast<u64>(-4));
    while (terminal.outbox_count) static_cast<void>(terminal.take_outbox());
    release(kernel, pid);
}

// Room, or a write that fails at once, wins over a pending signal.
void pty_write_readiness_wins(Kernel &kernel)
{
    for (const bool closed : {false, true})
    {
        u64 writer{}; const auto pid = linux_process(kernel, writer);
        const auto fds = make_pair(kernel, pid);
        caught(kernel, pid, 10, true);
        std::array<std::byte, sched::pipe_capacity> full{};
        activate(kernel, pid, pid);
        expect(kernel.posix().write(fds[0], full).value() == sched::pipe_capacity);
        std::memcpy(user(input_at), "abc", 3);
        enter(kernel, pid, writer, linux_write, fds[0], input_at, 3);
        expect(pending(kernel, pid, writer));
        if (closed) { activate(kernel, pid, pid); CHECK(kernel.posix().close(fds[1])); }
        else expect(take(kernel, pid, fds[1], 16) == 16);
        enqueue(kernel, pid, writer, 10);
        expect(!native::private_terminal_signal_ready(kernel, *kernel.scheduler().find(pid), thread_of(kernel, pid, writer)));
        resume(kernel, pid, writer);
        expect(!pending(kernel, pid, writer) && result_of(kernel, pid, writer) == (closed ? -5 : 3));
        resume(kernel, pid, writer);
        expect(thread_of(kernel, pid, writer).native_frame.pc == handler);
        release(kernel, pid);
    }
}

// An ignored signal and a default stop queued during the wait do not interrupt
// it; a caught signal queued after them does.
void pty_non_interrupting(Kernel &kernel)
{
    u64 reader{}; const auto pid = linux_process(kernel, reader);
    const auto fds = make_pair(kernel, pid);
    auto *ignored = native::private_signal_table(kernel, pid)->find(12); CHECK(ignored);
    *ignored = {}; ignored->handler = sched::SignalAction::ignored_handler;
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    auto &thread = thread_of(kernel, pid, reader);
    auto &process = *kernel.scheduler().find(pid);
    enqueue(kernel, pid, reader, OK_SIGTSTP);
    enqueue(kernel, pid, reader, 12);
    expect(native::private_terminal_signal_owned(kernel, process, thread));
    expect(!native::private_terminal_signal_ready(kernel, process, thread));
    expect(pending(kernel, pid, reader) && native::pty_wait_object(pid, reader).pool);
    caught(kernel, pid, 27, false);
    enqueue(kernel, pid, reader, 27);
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == -4 && !native::pty_wait_object(pid, reader).pool);
    release(kernel, pid);
}

// A Linux job in the console's foreground group (native init and login).
struct ConsoleJob { u64 login{}, job{}; i64 epoch{}; tty::Termios saved{}; };
i64 console_call(Kernel &kernel, u64 pid, N number, std::array<uptr, 6> arguments)
{
    activate(kernel, pid, pid);
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    expect(complete);
    return result;
}
ConsoleJob console_job(Kernel &kernel, u64 init)
{
    auto &scheduler = kernel.scheduler();
    scheduler.find(init)->resources().personality = sched::UserPersonality::native;
    ConsoleJob c{};
    c.login = fork(kernel, init);
    c.epoch = console_call(kernel, init, N::console_claim, {c.login});
    expect(c.epoch > 0 && console_call(kernel, c.login, N::setsid, {}) == static_cast<i64>(c.login));
    expect(console_call(kernel, c.login, N::ioctl, {0, OK_TIOCSCTTY}) == 0);
    c.job = fork(kernel, c.login);
    expect(console_call(kernel, c.login, N::setpgid, {c.job, c.job}) == 0);
    std::memcpy(user(argument_at), &c.job, 4);
    expect(console_call(kernel, c.login, N::ioctl, {0, OK_TIOCSPGRP, argument_at}) == 0);
    scheduler.find(c.job)->resources().personality = sched::UserPersonality::linux;
    for (const auto pid : {init, c.login}) for (auto &thread : scheduler.find(pid)->threads()) thread.native_active = false;
    c.saved = scheduler.console_terminal().termios;
    return c;
}
void console_done(Kernel &kernel, u64 init, const ConsoleJob &c)
{
    auto &scheduler = kernel.scheduler();
    scheduler.console_terminal().ldisc.flush_input();
    scheduler.console_terminal().termios = c.saved;
    for (const auto pid : {init, c.login}) for (auto &each : scheduler.find(pid)->threads()) each.native_active = true;
    release(kernel, c.job);
    expect(console_call(kernel, init, N::console_revoke, {static_cast<uptr>(c.epoch)}) == 0);
    release(kernel, c.login);
}

// A Linux console read in the foreground group: EINTR and SA_RESTART.
void console_read_signal(Kernel &kernel, u64 init, bool restart)
{
    auto &scheduler = kernel.scheduler();
    const auto c = console_job(kernel, init);
    const auto job = c.job;
    caught(kernel, job, 10, restart);
    enter(kernel, job, job, linux_read, 0, read_at, 16);
    auto &thread = thread_of(kernel, job, job);
    auto &process = *scheduler.find(job);
    expect(thread.syscall_pending && thread.wait_reason == sched::ThreadWaitReason::console_read && thread.console_wait_epoch);
    const auto origin = native::syscall_origins.identity(process, thread);
    const auto bound = native::syscall_origins.bound_console_read(origin, thread.native_frame);
    expect(bound && bound->epoch == thread.console_wait_epoch && bound->count == 16 && bound->bytes == 16 && !bound->vectored);
    expect(native::private_signal_admit_blocked(kernel, process, thread));
    // Input that does not complete the read is no readiness (canonical mode).
    auto &console = scheduler.console_terminal();
    console.termios = tty::linux_std_termios();
    native::input('x');
    // A wake that finds no complete line re-dispatches the read, which waits again.
    if (thread.wait_registration.active()) CHECK(scheduler.complete_wait(thread.wait_registration.ticket(), sched::WaitCompletion::ready));
    resume(kernel, job, job);
    expect(thread.syscall_pending && thread.wait_reason == sched::ThreadWaitReason::console_read);
    // The re-dispatched call rebound its new wait.
    const auto rebound = native::syscall_origins.bound_console_read(origin, thread.native_frame);
    expect(rebound && rebound->ticket.generation > bound->ticket.generation);
    enqueue(kernel, job, job, 10);
    expect(native::private_terminal_signal_ready(kernel, process, thread));
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && !thread.console_wait_epoch && thread.state != sched::ProcessState::blocked);
    expect(thread.native_frame.pc == (restart ? site : site + 2));
    expect(thread.native_frame.registers[14] == (restart ? linux_read : static_cast<u64>(-4)));
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    if (restart)
    {
        // The restarted read takes the line once it is complete.
        enter(kernel, job, job, linux_read, 0, read_at, 16);
        native::input('\r');
        resume(kernel, job, job);
        expect(!thread.syscall_pending && result_of(kernel, job, job) == 2 && *user(read_at) == std::byte{'x'});
    }
    console_done(kernel, init, c);
}

// Console readiness is the read plan of the call's bytes: a complete line, or
// min(VMIN, count) bytes, wins over a pending signal; a read below that gets
// EINTR and keeps its bytes (docs/modules/tty.md section 14, row 21). Each
// phase uses its own caught signal (a handler blocks its signal).
void console_read_ready(Kernel &kernel, u64 init)
{
    auto &scheduler = kernel.scheduler();
    const auto c = console_job(kernel, init);
    auto &console = scheduler.console_terminal();
    auto &thread = thread_of(kernel, c.job, c.job);
    auto &process = *scheduler.find(c.job);
    const auto wake = [&] {
        if (thread.wait_registration.active())
            CHECK(scheduler.complete_wait(thread.wait_registration.ticket(), sched::WaitCompletion::ready));
    };
    const auto read = [&](usize count, std::string_view input, u32 signal) {
        enter(kernel, c.job, c.job, linux_read, 0, read_at, count);
        expect(thread.syscall_pending && thread.wait_reason == sched::ThreadWaitReason::console_read);
        for (const char value : input) native::input(value);
        wake();
        caught(kernel, c.job, signal, false);
        enqueue(kernel, c.job, c.job, signal);
        expect(native::private_terminal_signal_owned(kernel, process, thread));
    };
    const auto completes = [&](i64 count, char first) {
        expect(!native::private_terminal_signal_ready(kernel, process, thread));
        resume(kernel, c.job, c.job);
        expect(!thread.syscall_pending && result_of(kernel, c.job, c.job) == count && *user(read_at) == std::byte(first));
        resume(kernel, c.job, c.job);
        expect(thread.native_frame.pc == handler);
    };
    console.termios = tty::linux_std_termios();
    read(16, "x\r", 10);
    completes(2, 'x');
    auto raw = tty::linux_std_termios();
    raw.lflag &= ~(tty::lflag::icanon | tty::lflag::echo);
    raw[tty::Cc::min] = 4;
    console.termios = raw;
    read(1, "y", 12);
    completes(1, 'y');
    read(4, "z", 14);
    expect(native::private_terminal_signal_ready(kernel, process, thread));
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(result_of(kernel, c.job, c.job) == -4 && console.ldisc.buffered() == 1);
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    console_done(kernel, init, c);
}

// VTIME: a VMIN 0 read returns 0 when its timer runs out; below VMIN the
// timer restarts on new input; a signal interrupts a timed wait.
void vtime(Kernel &kernel)
{
    u64 reader{}; const auto pid = linux_process(kernel, reader);
    const auto fds = make_pair(kernel, pid);
    auto &terminal = slave_terminal(kernel, pid, fds[1]);
    terminal.termios[tty::Cc::min] = 0; terminal.termios[tty::Cc::time] = 2;
    mock::now = 5000000000ULL;
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    auto object = native::pty_wait_object(pid, reader);
    expect(pending(kernel, pid, reader) && native::pty_wait_objects.resolve(object).value()->deadline == mock::now + 200000000ULL);
    mock::now += 199999999ULL;
    expect(native::expire_tty_timers(kernel, mock::now) && pending(kernel, pid, reader));
    mock::now += 1;
    resume(kernel, pid, reader);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 0 && !native::pty_wait_object(pid, reader).pool);
    // Input before the timer completes the read at once.
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    put_master(kernel, pid, fds[0], "q");
    resume(kernel, pid, reader);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 1);

    // Interbyte: VMIN 5, VTIME 1. No timer before the first byte.
    terminal.termios[tty::Cc::min] = 5; terminal.termios[tty::Cc::time] = 1;
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    object = native::pty_wait_object(pid, reader);
    expect(pending(kernel, pid, reader) && !native::pty_wait_objects.resolve(object).value()->deadline);
    put_master(kernel, pid, fds[0], "ab");
    resume(kernel, pid, reader);
    const auto first = native::pty_wait_objects.resolve(object).value()->deadline;
    expect(pending(kernel, pid, reader) && first == mock::now + 100000000ULL);
    mock::now += 50000000ULL;
    put_master(kernel, pid, fds[0], "c");
    resume(kernel, pid, reader);
    const auto second = native::pty_wait_objects.resolve(object).value()->deadline;
    expect(pending(kernel, pid, reader) && second == mock::now + 100000000ULL && second > first);
    mock::now += 60000000ULL;
    resume(kernel, pid, reader);
    expect(pending(kernel, pid, reader));
    mock::now += 40000000ULL;
    resume(kernel, pid, reader);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 3 && *user(read_at + 2) == std::byte{'c'});

    // The interbyte restart counts received bytes: input flushed and replaced
    // by as many new bytes still restarts the timer.
    terminal.termios[tty::Cc::time] = 10;
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    object = native::pty_wait_object(pid, reader);
    put_master(kernel, pid, fds[0], "ab");
    resume(kernel, pid, reader);
    expect(pending(kernel, pid, reader) && native::pty_wait_objects.resolve(object).value()->deadline == mock::now + 1000000000ULL);
    mock::now += 500000000ULL;
    expect(native_ioctl(kernel, pid, fds[1], OK_TCFLSH, 0) == 0 && !terminal.ldisc.buffered());
    mock::now += 400000000ULL;
    put_master(kernel, pid, fds[0], "cd");
    resume(kernel, pid, reader);
    expect(pending(kernel, pid, reader) && native::pty_wait_objects.resolve(object).value()->deadline == mock::now + 1000000000ULL);
    mock::now += 1000000000ULL;
    resume(kernel, pid, reader);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 2 && *user(read_at) == std::byte{'c'});

    // VTIME is taken when the read first waits: a TCSETS during the wait wakes
    // the reader but neither restarts nor shortens its timer (n_tty_read).
    terminal.termios[tty::Cc::min] = 0; terminal.termios[tty::Cc::time] = 50;
    const auto start = mock::now;
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    object = native::pty_wait_object(pid, reader);
    expect(pending(kernel, pid, reader) && native::pty_wait_objects.resolve(object).value()->deadline == start + 5000000000ULL);
    mock::now += 1000000000ULL;
    auto shorter = terminal.termios; shorter[tty::Cc::time] = 1;
    expect(set_termios(kernel, pid, fds[0], shorter) == 0 && terminal.termios[tty::Cc::time] == 1);
    resume(kernel, pid, reader);
    expect(pending(kernel, pid, reader) && native::pty_wait_objects.resolve(object).value()->deadline == start + 5000000000ULL);
    mock::now += 200000000ULL;
    resume(kernel, pid, reader);
    expect(pending(kernel, pid, reader));
    mock::now = start + 5000000000ULL;
    resume(kernel, pid, reader);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 0);

    // A caught signal interrupts a timed wait.
    terminal.termios[tty::Cc::min] = 0; terminal.termios[tty::Cc::time] = 50;
    caught(kernel, pid, 10, false);
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    enqueue(kernel, pid, reader, 10);
    expect(native::private_terminal_signal_complete(kernel, *kernel.scheduler().find(pid), thread_of(kernel, pid, reader)));
    expect(result_of(kernel, pid, reader) == -4 && !native::expire_tty_timers(kernel, mock::now));
    release(kernel, pid);
}

// A Linux background member of its PTY's session: a caught SIGTTIN interrupts
// its parked read with EINTR; a caught SIGTTOU under SA_RESTART restarts its
// parked TCSETS from the captured frame, which then completes in the foreground;
// a park whose signal another sender discarded re-runs its check.
void job_control_park(Kernel &kernel)
{
    auto &scheduler = kernel.scheduler();
    const auto leader = create(kernel);
    const auto fds = make_pair(kernel, leader);
    const auto *slave = scheduler.open_description(scheduler.find(leader)->fd_table().entry(fds[1])->description);
    const sched::TerminalRef ref{static_cast<u16>(slave->pty_endpoint.slot + 1), slave->pty_endpoint.generation};
    expect(scheduler.acquire_terminal(ref, *scheduler.find(leader), true, sched::AcquireMode::ioctl).ok());
    const auto member = fork(kernel, leader);
    CHECK(scheduler.set_process_group(leader, static_cast<i32>(member), static_cast<i32>(member)));
    auto &thread = thread_of(kernel, member, member);
    thread.native_active = true;
    thread_of(kernel, leader, leader).native_active = false;
    auto &process = *scheduler.find(member);
    expect(!scheduler.process_group_orphaned(member) && scheduler.terminal(ref)->foreground == leader);
    // The read parks in job control under the slave's key, bound to its origin.
    caught(kernel, member, OK_SIGTTIN, false);
    enter(kernel, member, member, linux_read, fds[1], read_at, 10);
    expect(pending(kernel, member, member) && thread.wait_reason == sched::ThreadWaitReason::job_control &&
           thread.wait_key == ref.index && !native::pty_wait_object(member, member).pool);
    auto origin = native::syscall_origins.identity(process, thread);
    const auto bound = native::syscall_origins.bound_terminal_control(origin, thread.native_frame);
    expect(bound && bound->ticket == thread.wait_registration.ticket() && bound->fd == fds[1] && bound->key == ref.index);
    expect(native::private_signal_admit_blocked(kernel, process, thread));
    expect(native::private_terminal_signal_ready(kernel, process, thread));
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && thread.native_frame.pc == site + 2 && thread.native_frame.registers[14] == static_cast<u64>(-4));
    expect(!native::syscall_origins.live(origin) && thread.state != sched::ProcessState::blocked);
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    // TCGETS is not checked; TCSETS belongs to the restartable terminal_control family.
    caught(kernel, member, OK_SIGTTOU, true);
    enter(kernel, member, member, linux_ioctl, fds[1], 0x5401, argument_at);
    expect(!pending(kernel, member, member) && result_of(kernel, member, member) == 0);
    enter(kernel, member, member, linux_ioctl, fds[1], 0x5402, argument_at);
    expect(pending(kernel, member, member) && thread.wait_reason == sched::ThreadWaitReason::job_control);
    expect(native::SyscallOrigins::Adapter::family(thread.native_frame) == native::SyscallOrigins::Adapter::Family::terminal_control);
    origin = native::syscall_origins.identity(process, thread);
    expect(native::syscall_origins.bound_terminal_control(origin, thread.native_frame).has_value());
    const auto frame = thread.native_frame;
    expect(native::private_terminal_signal_complete(kernel, process, thread));
    expect(!thread.syscall_pending && thread.native_frame.pc == site && thread.native_frame.registers[14] == linux_ioctl);
    for (usize index : {9, 10, 11}) expect(thread.native_frame.registers[index] == frame.registers[index]);
    expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    // In the foreground the restarted request completes.
    scheduler.terminal(ref)->foreground = member;
    enter(kernel, member, member, linux_ioctl, fds[1], 0x5402, argument_at);
    expect(!pending(kernel, member, member) && result_of(kernel, member, member) == 0);
    // A park whose signal is gone ends and re-runs its check (Linux restarts
    // the call): a SIGCONT from elsewhere discards the pending caught SIGTTIN
    // of the running member, and the re-dispatched read gets a new SIGTTIN
    // (EINTR from its handler), or EIO once SIGTTIN is ignored.
    scheduler.terminal(ref)->foreground = leader;
    for (const bool ignore : {false, true})
    {
        thread.native_signal_mask = {}; // The handlers above returned.
        caught(kernel, member, OK_SIGTTIN, false);
        enter(kernel, member, member, linux_read, fds[1], read_at, 10);
        expect(pending(kernel, member, member) && thread.wait_reason == sched::ThreadWaitReason::job_control);
        native::private_jobctl_settle(kernel);
        expect(thread.state == sched::ProcessState::blocked && native::private_terminal_signal_ready(kernel, process, thread));
        static_cast<void>(native::send_kernel_process_signal(kernel, member, OK_SIGCONT));
        expect(!native::private_terminal_signal_ready(kernel, process, thread) && thread.state == sched::ProcessState::blocked);
        if (ignore) native::private_signal_table(kernel, member)->find(OK_SIGTTIN)->handler = sched::SignalAction::ignored_handler;
        native::private_jobctl_settle(kernel);
        expect(thread.state == sched::ProcessState::runnable && pending(kernel, member, member));
        resume(kernel, member, member);
        if (ignore)
        {
            expect(!pending(kernel, member, member) && result_of(kernel, member, member) == -5);
            continue;
        }
        expect(pending(kernel, member, member) && thread.wait_reason == sched::ThreadWaitReason::job_control);
        expect(native::private_terminal_signal_complete(kernel, process, thread) && result_of(kernel, member, member) == -4);
        expect(native::private_signal_deliver(kernel, process, thread) && thread.native_frame.pc == handler);
    }
    thread_of(kernel, leader, leader).native_active = true;
    release(kernel, member);
    release(kernel, leader);
}

// A vectored Linux read and a nonblocking one through the same owners.
void vectors(Kernel &kernel)
{
    u64 reader{}; const auto pid = linux_process(kernel, reader);
    const auto fds = make_pair(kernel, pid);
    const std::array<native::PipeVector, 2> parts{{{read_at, 2}, {read_at + 16, 8000}}};
    std::memcpy(user(vectors_at), parts.data(), sizeof(parts));
    put_master(kernel, pid, fds[0], "vector");
    enter(kernel, pid, reader, linux_readv, fds[1], vectors_at, 2);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == 6);
    expect(*user(read_at + 1) == std::byte{'e'} && *user(read_at + 16) == std::byte{'c'});
    activate(kernel, pid, pid);
    CHECK(kernel.posix().fcntl(fds[1], posix::f_SETFL, posix::o_NONBLOCK));
    enter(kernel, pid, reader, linux_read, fds[1], read_at, 10);
    expect(!pending(kernel, pid, reader) && result_of(kernel, pid, reader) == -11);
    release(kernel, pid);
}

// Linux read, write and ppoll of PTY endpoints on every profile.
template <lu::Profile P>
usize profile_io(Kernel &kernel, usize number_slot, std::array<int, 5> slots)
{
    using C = lu::Codec<P>;
    constexpr bool big = C::profile.big_endian;
    const auto pid = create(kernel);
    const auto fds = make_pair(kernel, pid);
    auto put = [&](uptr address, usize width, u64 value) {
        for (usize i = 0; i < width; ++i) *user(address + i) = static_cast<std::byte>(value >> (8 * (big ? width - i - 1 : i)));
    };
    auto get = [&](uptr address, usize width) {
        u64 value{};
        for (usize i = 0; i < width; ++i) value |= u64(std::to_integer<u8>(*user(address + i))) << (8 * (big ? width - i - 1 : i));
        return value;
    };
    auto call = [&](lu::Operation operation, std::array<u64, 5> arguments) {
        const auto number = C::syscall_number(operation); CHECK(number);
        hw::UserFrame frame{};
        frame.registers[number_slot] = *number;
        frame.sp = hw::user_begin + 0x3800;
        for (usize i = 0; i < arguments.size(); ++i)
            if (slots[i] >= 0) frame.registers[slots[i]] = static_cast<uptr>(arguments[i]);
            else put(frame.sp + 16, 4, arguments[i]);
        activate(kernel, pid, pid);
        auto &thread = thread_of(kernel, pid, pid);
        thread.syscall_pending = true;
        bool done = true;
        ++linux_calls;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        thread.syscall_pending = false;
        if (!done) std::fprintf(stderr, "%s pending %lld\n", C::profile.name, static_cast<long long>(result));
        expect(done);
        return result;
    };
    const auto ppoll = C::syscall_number(lu::Operation::ppoll) ? lu::Operation::ppoll : lu::Operation::ppoll_time64;
    const usize width = ppoll == lu::Operation::ppoll_time64 ? 8 : C::profile.word_bytes;
    auto poll = [&](i32 fd, u16 events) {
        put(poll_at, 4, static_cast<u32>(fd)); put(poll_at + 4, 2, events); put(poll_at + 6, 2, 0x5555);
        put(time_at, width, 0); put(time_at + width, width, 0);
        const auto result = call(ppoll, {poll_at, 1, time_at, 0, 0});
        expect(result == (get(poll_at + 6, 2) ? 1 : 0));
        return static_cast<u16>(get(poll_at + 6, 2));
    };
    constexpr u16 in = 1, out = 4, hup = 0x10;
    expect(poll(fds[1], in | out) == out);
    std::memcpy(user(input_at), "pty", 3);
    expect(call(lu::Operation::write, {static_cast<u64>(fds[0]), input_at, 3, 0, 0}) == 3);
    expect(poll(fds[1], in | out) == (in | out));
    std::memset(user(read_at), 0, 4);
    expect(call(lu::Operation::read, {static_cast<u64>(fds[1]), read_at, 4096 + 100, 0, 0}) == 3);
    expect(std::memcmp(user(read_at), "pty", 3) == 0);
    expect(call(lu::Operation::write, {static_cast<u64>(fds[1]), input_at, 2, 0, 0}) == 2);
    expect(poll(fds[0], in) == in);
    expect(call(lu::Operation::read, {static_cast<u64>(fds[0]), read_at, 8, 0, 0}) == 2);
    // A raw pair's master after the slave's close: IN|HUP, then EIO.
    activate(kernel, pid, pid);
    CHECK(kernel.posix().close(fds[1]));
    expect(poll(fds[0], in | out) == (in | hup));
    expect(call(lu::Operation::read, {static_cast<u64>(fds[0]), read_at, 8, 0, 0}) == -5);
    release(kernel, pid);
    return 1;
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    mock::spaces_enabled = true;
    const usize profiles =
        profile_io<lu::Profile::i386>(kernel, 7, {4, 6, 5, 1, 0}) +
        profile_io<lu::Profile::x86_64>(kernel, 14, {9, 10, 11, 5, 7}) +
        profile_io<lu::Profile::aarch64>(kernel, 8, {0, 1, 2, 3, 4}) +
        profile_io<lu::Profile::arm32>(kernel, 7, {0, 1, 2, 3, 4}) +
        profile_io<lu::Profile::rv64>(kernel, 17, {10, 11, 12, 13, 14}) +
        profile_io<lu::Profile::rv32>(kernel, 17, {10, 11, 12, 13, 14}) +
        profile_io<lu::Profile::loongarch64>(kernel, 11, {4, 5, 6, 7, 8}) +
        profile_io<lu::Profile::mips>(kernel, 2, {4, 5, 6, 7, -1}) +
        profile_io<lu::Profile::mips64>(kernel, 2, {4, 5, 6, 7, 8}) +
        profile_io<lu::Profile::ppc>(kernel, 0, {3, 4, 5, 6, 7});
    const auto sender = create(kernel);
    pty_read_signal(kernel, sender, false);
    pty_read_signal(kernel, sender, true);
    pty_read_step(kernel);
    pty_ready_and_default(kernel);
    pty_write_signal(kernel);
    pty_write_expansion(kernel, sender);
    pty_master_signal_character(kernel);
    pty_write_readiness_wins(kernel);
    pty_non_interrupting(kernel);
    const auto init = create_init(kernel);
    console_read_signal(kernel, init, false);
    console_read_signal(kernel, init, true);
    console_read_ready(kernel, init);
    vtime(kernel);
    vectors(kernel);
    job_control_park(kernel);
    release(kernel, sender);
    const auto stats = native::pty_wait_objects.accounting();
    expect(stats.allocations == stats.destructions && !stats.live_objects);
    expect(native::runtime_status().ok());
    std::printf("NATIVE_TERMINAL_SIGNAL_RUNTIME_PASS linux_calls=%zu profiles=%zu eintr=1 restart=1 partial_write=1 "
                "console_read=1 poll=1 vtime=1 job_control_park=1 hosted=1 guest=0\n", linux_calls, profiles);
}
