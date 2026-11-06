// Terminals in the actual runtime. The console: N_TTY ingress, echo and OPOST
// output observed through the platform console callback, ISIG and SIGWINCH as
// kernel signals, flushes, large writev and prepared reads. PTYs: slave
// acquisition, ISIG and SIGWINCH from either end, canonical echo into the
// master, raw-pair drain, TIOCGPTN/TIOCSPTLCK and the ptmx hangup. Hosted trap
// and mapping backends; no guest execution.
#define OK_NATIVE_MODEL_MEMORY_BYTES 32768
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"
#include <string>

namespace
{
std::string console_output;
}
extern "C" void ok_platform_console_write_char(char value) { console_output += value; }

namespace
{
using N = syscall::Number;
using Wire = tty::NativeTermiosWire<false>;
constexpr uptr argument_at = hw::user_begin + 0x2400, vectors_at = hw::user_begin + 0x2800,
               read_at = hw::user_begin + 0x2c00, text_at = hw::user_begin + 0x3000;

std::byte *user(uptr address) { return mock::memory.data() + (address - hw::user_begin); }

i64 native_call(Kernel &kernel, u64 pid, N number, std::array<uptr, 6> arguments, bool *pending = nullptr)
{
    activate(kernel, pid, pid);
    auto *thread = kernel.scheduler().find_thread(pid, pid);
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    if (pending) *pending = !complete;
    else expect(complete);
    if (complete) thread->console_wait_epoch = 0;
    return result;
}

i64 ioctl(Kernel &kernel, u64 pid, u64 request, uptr argument = 0)
{
    return native_call(kernel, pid, N::ioctl, {0, static_cast<uptr>(request), argument});
}

void set_termios(Kernel &kernel, u64 pid, const tty::Termios &termios, u64 request = OK_TCSETS)
{
    std::array<u8, 36> bytes{};
    expect(Wire::encode(termios, {}, bytes).ok());
    std::memcpy(user(argument_at), bytes.data(), bytes.size());
    expect(ioctl(kernel, pid, request, argument_at) == 0);
}

void type(std::string_view text)
{
    for (const char value : text) native::input(value);
}

std::string read_text(Kernel &kernel, u64 pid, usize count = 64,
                      std::source_location at = std::source_location::current())
{
    const auto result = native_call(kernel, pid, N::read, {0, read_at, count});
    if (result < 0) std::fprintf(stderr, "read %lld\n", static_cast<long long>(result));
    expect(result >= 0, at);
    return {reinterpret_cast<const char *>(user(read_at)), static_cast<usize>(result > 0 ? result : 0)};
}

i64 write_text(Kernel &kernel, u64 pid, std::string_view text)
{
    std::memcpy(user(text_at), text.data(), text.size());
    return native_call(kernel, pid, N::write, {1, text_at, text.size()});
}

std::string take_output()
{
    auto result = console_output;
    console_output.clear();
    return result;
}

void handle(Kernel &kernel, u64 pid, u32 signal)
{
    auto *entry = native::private_signal_table(kernel, pid)->find(signal); CHECK(entry);
    *entry = {}; entry->handler = handler; entry->flags = 0x04000004; entry->restorer = handler + 4;
}

// The oldest pending shared signal of `pid`, consumed; 0 when none.
u32 take_signal(u64 pid, i32 *code = nullptr)
{
    native::PrivateProcessQueue::Delivery ticket;
    auto *state = native::private_signal_find(pid);
    if (!state) return 0;
    auto &queue = state->shared;
    if (!queue.peek({}, ticket)) return 0;
    if (code) *code = ticket.info.code;
    CHECK(queue.consume(ticket));
    return ticket.info.signal;
}

struct Session
{
    u64 init{}, login{}, job{};
    u32 epoch{};
};

Session open_console(Kernel &kernel)
{
    Session session{};
    session.init = create_init(kernel);
    auto &scheduler = kernel.scheduler();
    scheduler.find(session.init)->resources().personality = sched::UserPersonality::native;
    session.login = fork(kernel, session.init);
    scheduler.find(session.login)->threads()[0].native_active = true;
    const auto epoch = native_call(kernel, session.init, N::console_claim, {session.login});
    expect(epoch > 0);
    session.epoch = static_cast<u32>(epoch);
    expect(native_call(kernel, session.login, N::setsid, {}) == static_cast<i64>(session.login));
    expect(ioctl(kernel, session.login, OK_TIOCSCTTY) == 0);
    // A Linux job in its own foreground group, catching INT, QUIT and WINCH.
    session.job = fork(kernel, session.login);
    scheduler.find(session.job)->threads()[0].native_active = true;
    expect(native_call(kernel, session.login, N::setpgid, {session.job, session.job}) == 0);
    scheduler.find(session.job)->resources().personality = sched::UserPersonality::linux;
    for (const u32 signal : {u32{OK_SIGINT}, u32{OK_SIGQUIT}, u32{OK_SIGWINCH}}) handle(kernel, session.job, signal);
    return session;
}

// The compat default reproduces the raw console: CR becomes LF, no echo, no
// ONLCR, bytes readable as they come.
bool compat_default(Kernel &kernel, const Session &s)
{
    auto &console = kernel.scheduler().console_terminal();
    expect(console.termios == tty::console_compat_termios() && console.window == tty::console_window());
    mock::memory.fill(std::byte{});
    expect(ioctl(kernel, s.login, OK_TCGETS, argument_at) == 0);
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    expect(Wire::decode({reinterpret_cast<const u8 *>(user(argument_at)), 36}, termios, residue).ok());
    expect(termios == tty::console_compat_termios());
    take_output();
    type("ab\r\x7f\x03");
    expect(console_output.empty());
    expect(read_text(kernel, s.login, 2) == "ab" && read_text(kernel, s.login) == "\n\x7f\x03");
    expect(write_text(kernel, s.login, "x\ny\tz") == 5 && take_output() == "x\ny\tz");
    return true;
}

bool icanon_echo(Kernel &kernel, const Session &s)
{
    set_termios(kernel, s.login, tty::linux_std_termios());
    take_output();
    // Erase echoes "\b \b"; CR is a newline (ICRNL) echoed as CR LF (ONLCR).
    type("helx\x7flo\r");
    expect(take_output() == "helx\b \blo\r\n");
    expect(read_text(kernel, s.login) == "hello\n");
    // A partial line does not satisfy a canonical read; its end wakes the reader.
    type("ab");
    bool pending = false;
    expect(native_call(kernel, s.login, N::read, {0, read_at, 64}, &pending) == -11 && pending);
    const auto *reader = kernel.scheduler().find_thread(s.login, s.login);
    expect(reader->state == sched::ProcessState::blocked && reader->wait_reason == sched::ThreadWaitReason::console_read);
    type("\n");
    expect(reader->state == sched::ProcessState::runnable && take_output() == "ab\r\n");
    expect(read_text(kernel, s.login) == "ab\n");
    // VEOF at the start of a line reads 0; output goes through OPOST.
    type("\x04");
    expect(native_call(kernel, s.login, N::read, {0, read_at, 64}) == 0);
    take_output();
    expect(write_text(kernel, s.login, "a\nb") == 3 && take_output() == "a\r\nb");
    // Pending echo leaves before program output (n_tty_write).
    auto &console = kernel.scheduler().console_terminal();
    const std::array<std::byte, 1> byte{std::byte{'q'}};
    expect(console.ldisc.receive(console.termios, byte, 0, true).echoed && console.ldisc.echo_pending() == 1);
    expect(write_text(kernel, s.login, "!") == 1 && take_output() == "q!" && !console.ldisc.echo_pending());
    console.ldisc.flush_input();
    return true;
}

bool isig(Kernel &kernel, const Session &s)
{
    auto &console = kernel.scheduler().console_terminal();
    i32 code{};
    std::memcpy(user(argument_at), &s.job, 4);
    expect(ioctl(kernel, s.login, OK_TIOCSPGRP, argument_at) == 0);
    type("abc\x03");
    expect(take_output() == "abc^C" && !console.ldisc.buffered() && !console.outbox_count);
    expect(take_signal(s.job, &code) == OK_SIGINT && code == 128 && !take_signal(s.job));
    expect(!take_signal(s.login));
    type("\x1c");
    expect(take_signal(s.job, &code) == OK_SIGQUIT && code == 128);
    // NOFLSH keeps the input; ISIG off makes ^C an ordinary byte.
    auto termios = tty::linux_std_termios();
    termios.lflag |= tty::lflag::noflsh;
    set_termios(kernel, s.login, termios);
    type("k\x03");
    expect(console.ldisc.buffered() == 1 && take_signal(s.job) == OK_SIGINT);
    termios.lflag &= ~tty::lflag::isig;
    set_termios(kernel, s.login, termios);
    type("\x03\n");
    expect(!take_signal(s.job) && read_text(kernel, s.job) == "k\x03\n");
    take_output();
    // ^Z stops the foreground job (SIGTSTP default); SIGCONT resumes it.
    set_termios(kernel, s.login, tty::linux_std_termios());
    type("\x1a");
    expect(kernel.scheduler().find(s.job)->job_control().stopped && take_output() == "^Z");
    static_cast<void>(native::send_kernel_process_signal(kernel, s.job, OK_SIGCONT)); // Continues, then ignored (SIG_DFL).
    expect(!kernel.scheduler().find(s.job)->job_control().stopped);
    while (take_signal(s.job)) {}
    return true;
}

bool winch(Kernel &kernel, const Session &s)
{
    std::array<u16, 4> window{40, 100, 0, 0};
    std::memcpy(user(argument_at), window.data(), sizeof(window));
    expect(ioctl(kernel, s.login, OK_TIOCSWINSZ, argument_at) == 0);
    expect(take_signal(s.job) == OK_SIGWINCH && !take_signal(s.login));
    expect(ioctl(kernel, s.login, OK_TIOCSWINSZ, argument_at) == 0 && !take_signal(s.job)); // Unchanged size.
    mock::memory.fill(std::byte{});
    expect(ioctl(kernel, s.job, OK_TIOCGWINSZ, argument_at) == 0);
    expect(std::memcmp(user(argument_at), window.data(), sizeof(window)) == 0);
    // The Linux job may resize too (Linux TIOCSWINSZ is not job-control checked).
    window = {24, 80, 0, 0};
    std::memcpy(user(argument_at), window.data(), sizeof(window));
    expect(ioctl(kernel, s.job, OK_TIOCSWINSZ, argument_at) == 0 && take_signal(s.job) == OK_SIGWINCH);
    expect(kernel.scheduler().console_terminal().window == tty::console_window());
    return true;
}

bool flush(Kernel &kernel, const Session &s)
{
    auto &console = kernel.scheduler().console_terminal();
    std::memcpy(user(argument_at), &s.login, 4);
    expect(ioctl(kernel, s.login, OK_TIOCSPGRP, argument_at) == 0);
    // A cooked line typed before the prompt does not survive OK_SYS_CONSOLE_FLUSH.
    type("stale\n");
    expect(console.ldisc.inq(console.termios) == 6);
    expect(native_call(kernel, s.login, N::console_flush, {s.epoch}) == 0 && !console.ldisc.buffered());
    type("x\n");
    expect(ioctl(kernel, s.login, OK_TCFLSH, 0) == 0 && !console.ldisc.buffered());
    type("y\n");
    set_termios(kernel, s.login, tty::linux_std_termios(), OK_TCSETSF);
    expect(!console.ldisc.buffered());
    type("kept\n");
    expect(read_text(kernel, s.login) == "kept\n");
    take_output();
    return true;
}

// Repeated 8 KiB writevs return their full count (the console capture used to
// report EOVERFLOW after about 4 KiB); a write also takes 8 KiB at once.
bool writev_8k(Kernel &kernel, const Session &s)
{
    set_termios(kernel, s.login, tty::console_compat_termios());
    for (usize index = 0; index < 8192; ++index) *user(text_at + index) = std::byte{static_cast<u8>('a' + index % 26)};
    const std::array<native::PipeVector, 2> vectors{{{text_at, 4096}, {text_at + 4096, 4096}}};
    std::memcpy(user(vectors_at), vectors.data(), sizeof(vectors));
    take_output();
    for (usize round = 0; round < 3; ++round)
    {
        expect(native_call(kernel, s.login, N::writev, {1, vectors_at, 2}) == 8192);
        const auto output = take_output();
        expect(output.size() == 8192 && output.front() == 'a' && output.back() == static_cast<char>('a' + 8191 % 26));
    }
    expect(native_call(kernel, s.login, N::write, {2, text_at, 8192}) == 8192 && take_output().size() == 8192);
    // Linux writev (x86_64 number 20) reaches the same path.
    activate(kernel, s.job, s.job);
    hw::UserFrame frame{};
    frame.registers[14] = 20; frame.registers[9] = 1; frame.registers[10] = vectors_at; frame.registers[11] = 2;
    bool complete = true;
    expect(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel, s.job, frame, complete,
        [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, s.job, request, done); }) == 8192);
    expect(complete && take_output().size() == 8192);
    // ONLCR expands without splitting CR LF across chunks.
    set_termios(kernel, s.login, tty::linux_std_termios());
    for (usize index = 0; index < 8192; ++index) *user(text_at + index) = std::byte{'\n'};
    expect(native_call(kernel, s.login, N::writev, {1, vectors_at, 2}) == 8192);
    const auto expanded = take_output();
    expect(expanded.size() == 16384 && expanded.substr(0, 4) == "\r\n\r\n" && expanded.find("\n\n") == std::string::npos);
    // A bad vector is refused before anything is written.
    const std::array<native::PipeVector, 2> bad{{{text_at, 4}, {hw::user_end, 4}}};
    std::memcpy(user(vectors_at), bad.data(), sizeof(bad));
    expect(native_call(kernel, s.login, N::writev, {1, vectors_at, 2}) == -14 && console_output.empty());
    return true;
}

// The copy-out happens before the input is consumed.
bool efault_keeps_input(Kernel &kernel, const Session &s)
{
    set_termios(kernel, s.login, tty::console_compat_termios());
    type("qr");
    mock::partial_output = 1;
    expect(native_call(kernel, s.login, N::read, {0, read_at, 2}) == -14);
    mock::partial_output = 0;
    expect(kernel.scheduler().console_terminal().ldisc.buffered() == 2);
    // readv: the copy-out faults after the range checks passed.
    const std::array<native::PipeVector, 2> vectors{{{read_at, 1}, {read_at + 8, 1}}};
    std::memcpy(user(vectors_at), vectors.data(), sizeof(vectors));
    mock::partial_output = 1;
    expect(native_call(kernel, s.login, N::readv, {0, vectors_at, 2}) == -14);
    mock::partial_output = 0;
    expect(kernel.scheduler().console_terminal().ldisc.buffered() == 2);
    expect(native_call(kernel, s.login, N::readv, {0, vectors_at, 2}) == 2);
    expect(*user(read_at) == std::byte{'q'} && *user(read_at + 8) == std::byte{'r'});
    return true;
}
// PTY descriptors in the actual runtime. Calls run with a pending syscall, as
// PTY I/O continuations require.
i64 pty_call(Kernel &kernel, u64 pid, N number, std::array<uptr, 6> arguments)
{
    activate(kernel, pid, pid);
    auto *thread = kernel.scheduler().find_thread(pid, pid);
    thread->syscall_pending = true;
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    expect(complete);
    thread->syscall_pending = false;
    return result;
}
i64 fd_ioctl(Kernel &kernel, u64 pid, i32 fd, u64 request, uptr argument = 0)
{
    return pty_call(kernel, pid, N::ioctl, {static_cast<uptr>(fd), static_cast<uptr>(request), argument});
}
std::array<i32, 2> raw_pair(Kernel &kernel, u64 pid)
{
    expect(pty_call(kernel, pid, N::pty_pair, {argument_at, 0}) == 0);
    std::array<i32, 2> fds{};
    std::memcpy(fds.data(), user(argument_at), sizeof(fds));
    return fds;
}
i64 pty_write(Kernel &kernel, u64 pid, i32 fd, std::string_view text)
{
    std::memcpy(user(text_at), text.data(), text.size());
    return pty_call(kernel, pid, N::write, {static_cast<uptr>(fd), text_at, text.size()});
}
std::string pty_read(Kernel &kernel, u64 pid, i32 fd, usize count = 64)
{
    const auto result = pty_call(kernel, pid, N::read, {static_cast<uptr>(fd), read_at, count});
    expect(result >= 0);
    return {reinterpret_cast<const char *>(user(read_at)), static_cast<usize>(result > 0 ? result : 0)};
}
i16 pty_poll(Kernel &kernel, u64 pid, i32 fd, i16 events)
{
    ok_pollfd request{fd, events, 0};
    std::memcpy(user(argument_at), &request, sizeof(request));
    expect(pty_call(kernel, pid, N::poll, {argument_at, 1, 0}) >= 0);
    std::memcpy(&request, user(argument_at), sizeof(request));
    return request.revents;
}
// The S6 devfs open path in miniature: a fresh description, pinned, bound,
// installed at the lowest free fd and unpinned.
template <typename Bind> i32 bind_fd(Kernel &kernel, u64 pid, Bind bind)
{
    auto &scheduler = kernel.scheduler();
    const auto made = scheduler.allocate_description({.readable = true, .writable = true, .flags = 2u}); CHECK(made);
    CHECK(scheduler.retain_description(made.value()));
    auto &resources = scheduler.find(pid)->resources();
    i32 fd = 0;
    while (resources.files.contains(fd)) ++fd;
    const bool bound = static_cast<bool>(bind(made.value()));
    if (bound) CHECK(scheduler.install_descriptor(resources, fd, made.value()));
    CHECK(scheduler.release_description(made.value()));
    CHECK(scheduler.terminal_audit());
    return bound ? fd : -1;
}
i32 int_at(uptr address) { i32 value{}; std::memcpy(&value, user(address), 4); return value; }

struct PtySession
{
    u64 shell{}, job{};
};

// A session leader acquires a raw pair's slave; ISIG and SIGWINCH reach its
// foreground job; the master is never a controlling terminal.
PtySession pty_session(Kernel &kernel, const Session &s, std::array<i32, 2> &fds)
{
    auto &scheduler = kernel.scheduler();
    PtySession p{};
    p.shell = fork(kernel, s.init);
    scheduler.find(p.shell)->threads()[0].native_active = true;
    expect(native_call(kernel, p.shell, N::setsid, {}) == static_cast<i64>(p.shell));
    fds = raw_pair(kernel, p.shell);
    // v1: TIOCGPGRP on either raw end of an unassociated pair is ENOTTY.
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCGPGRP, argument_at) == -25);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCGPGRP, argument_at) == -25);
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCSCTTY) == -25);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCGSID, argument_at) == -25); // Not yet its ctty.
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCSCTTY) == 0);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCSCTTY) == 0); // Already this leader's terminal.
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCGSID, argument_at) == 0 && int_at(argument_at) == static_cast<i32>(p.shell));
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCGPGRP, argument_at) == 0 && int_at(argument_at) == static_cast<i32>(p.shell));
    // A session leader with a terminal cannot take the console or another PTY.
    const auto other = raw_pair(kernel, p.shell);
    expect(fd_ioctl(kernel, p.shell, other[1], OK_TIOCSCTTY) == -1);
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(other[0])}) == 0);
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(other[1])}) == 0);
    p.job = fork(kernel, p.shell);
    scheduler.find(p.job)->threads()[0].native_active = true;
    expect(native_call(kernel, p.shell, N::setpgid, {p.job, p.job}) == 0);
    scheduler.find(p.job)->resources().personality = sched::UserPersonality::linux;
    for (const u32 signal : {u32{OK_SIGINT}, u32{OK_SIGWINCH}}) handle(kernel, p.job, signal);
    std::memcpy(user(argument_at), &p.job, 4);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCSPGRP, argument_at) == 0);
    return p;
}

bool pty_terminal(Kernel &kernel, const Session &s, PtySession &p, std::array<i32, 2> &fds)
{
    p = pty_session(kernel, s, fds);
    // Raw pairs start with raw termios; the master reads and sets its slave's.
    mock::memory.fill(std::byte{});
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TCGETS, argument_at) == 0);
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    expect(Wire::decode({reinterpret_cast<const u8 *>(user(argument_at)), 36}, termios, residue).ok());
    expect(termios == tty::raw_pair_termios());
    std::array<u8, 36> bytes{};
    expect(Wire::encode(tty::linux_pty_termios(), {}, bytes).ok());
    std::memcpy(user(argument_at), bytes.data(), bytes.size());
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TCSETS, argument_at) == 0);
    const auto subject = kernel.posix().terminal_descriptor(fds[1]); CHECK(subject);
    auto *terminal = kernel.scheduler().terminal(subject.value().terminal);
    expect(terminal->termios == tty::linux_pty_termios() && terminal->foreground == p.job);
    // ISIG from master input goes to the foreground job at once.
    expect(pty_write(kernel, p.shell, fds[0], "ab\x03") == 3);
    i32 code{};
    expect(take_signal(p.job, &code) == OK_SIGINT && code == 128 && !take_signal(p.shell) && !terminal->outbox_count);
    // The flush discards the uncommitted echo of "ab" (Linux isig reset_buffer_flags).
    expect(pty_read(kernel, p.shell, fds[0]) == "^C");
    // More signal characters than the outbox holds finish in one blocking write,
    // and a write refused by a full outbox goes on once the runtime drained it:
    // no later event would wake a writer parked there.
    expect(pty_write(kernel, p.shell, fds[0], std::string(9, '\x03')) == 9);
    usize interrupts = 0;
    for (u32 signal = take_signal(p.job); signal; signal = take_signal(p.job)) interrupts += signal == OK_SIGINT ? 1 : 100;
    expect(interrupts >= 1 && interrupts < 100 && !terminal->outbox_count);
    for (const auto text : {std::string_view{"\x03"}, std::string_view{"k\x03"}})
    {
        for (sched::ProcessId target = 9000; terminal->outbox_count < sched::terminal_outbox_capacity; ++target)
            expect(terminal->owe(target, tty::TerminalSignal::winch, false));
        expect(pty_write(kernel, p.shell, fds[0], text) == static_cast<i64>(text.size()) && take_signal(p.job) == OK_SIGINT);
        expect(!terminal->outbox_count && pty_read(kernel, p.shell, fds[0]).ends_with("^C"));
    }
    // SIGWINCH on a changed size, through either end.
    std::array<u16, 4> window{30, 90, 0, 0};
    std::memcpy(user(argument_at), window.data(), sizeof(window));
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCSWINSZ, argument_at) == 0);
    expect(take_signal(p.job) == OK_SIGWINCH);
    mock::memory.fill(std::byte{});
    expect(fd_ioctl(kernel, p.job, fds[1], OK_TIOCGWINSZ, argument_at) == 0);
    expect(std::memcmp(user(argument_at), window.data(), sizeof(window)) == 0);
    // FIONREAD per side; TIOCGPTN is refused on a raw master and on a slave.
    expect(pty_write(kernel, p.shell, fds[1], "out") == 3);
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_FIONREAD, argument_at) == 0 && int_at(argument_at) == 3);
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCGPTN, argument_at) == -25);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TIOCGPTN, argument_at) == -25);
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TIOCSPTLCK, 0) == -25); // ENOTTY precedes the copy-in.
    expect(fd_ioctl(kernel, p.shell, fds[0], OK_TCFLSH, 2) == 0 && pty_poll(kernel, p.shell, fds[0], OK_POLLIN) == 0);
    return true;
}

// PTY slave readers follow the console wake rule: every stored byte in
// non-canonical mode, a completed line in canonical mode, every termios change.
bool pty_wakes(Kernel &kernel, const PtySession &p, const std::array<i32, 2> &fds)
{
    auto &scheduler = kernel.scheduler();
    const auto *reader = scheduler.find_thread(p.shell, p.shell);
    const auto master = scheduler.find(p.shell)->resources().files.entry(fds[0])->description;
    const auto blocked = [&] {
        return reader->state == sched::ProcessState::blocked && reader->wait_reason == sched::ThreadWaitReason::pty_read;
    };
    const auto block = [&] {
        activate(kernel, p.shell, p.shell);
        scheduler.find_thread(p.shell, p.shell)->syscall_pending = true;
        bool complete = true;
        expect(native::dispatch(kernel, p.shell, {static_cast<uptr>(N::read), {static_cast<uptr>(fds[1]), read_at, 10}},
                                complete) == -11 && !complete && blocked());
    };
    // A woken reader re-plans: -11 when still unmet, else the bytes.
    const auto resume = [&] {
        activate(kernel, p.shell, p.shell);
        bool complete = true;
        const auto result = native::dispatch(kernel, p.shell, {static_cast<uptr>(N::read), {~uptr{0}, 0, 0}}, complete);
        if (complete) scheduler.find_thread(p.shell, p.shell)->syscall_pending = false;
        return result;
    };
    const auto master_input = [&](std::string_view text) {
        expect(scheduler.write_pty_description(master, {reinterpret_cast<const std::byte *>(text.data()), text.size()})
                   .value() == text.size());
    };
    const auto set = [&](const tty::Termios &termios) {
        std::array<u8, 36> bytes{};
        expect(Wire::encode(termios, {}, bytes).ok());
        std::memcpy(user(argument_at), bytes.data(), bytes.size());
        expect(fd_ioctl(kernel, p.job, fds[0], OK_TCSETS, argument_at) == 0); // Through the master, by another process.
    };
    auto raw = tty::linux_pty_termios();
    raw.lflag &= ~(tty::lflag::icanon | tty::lflag::echo);
    raw[tty::Cc::min] = 3;
    set(raw);
    block();
    master_input("a"); expect(!blocked() && resume() == -11 && blocked());
    master_input("b"); expect(!blocked() && resume() == -11 && blocked());
    master_input("c"); expect(!blocked() && resume() == 3);
    expect(std::string_view{reinterpret_cast<const char *>(user(read_at)), 3} == "abc");
    // Canonical: a partial line leaves the reader asleep; the newline wakes it.
    auto cooked = tty::linux_pty_termios();
    cooked.lflag &= ~tty::lflag::echo;
    set(cooked);
    block();
    master_input("xy"); expect(blocked());
    master_input("\n"); expect(!blocked() && resume() == 3);
    // Lowering VMIN to the buffered count wakes the reader.
    raw[tty::Cc::min] = 5;
    set(raw);
    master_input("pq");
    block();
    raw[tty::Cc::min] = 2;
    set(raw);
    expect(!blocked() && resume() == 2);
    expect(std::string_view{reinterpret_cast<const char *>(user(read_at)), 2} == "pq");
    set(tty::linux_pty_termios());
    return true;
}

bool canonical_echo(Kernel &kernel, const PtySession &p, const std::array<i32, 2> &fds)
{
    expect(pty_write(kernel, p.shell, fds[0], "hel\x7flo\r") == 7);
    expect(pty_read(kernel, p.shell, fds[0]) == "hel\b \blo\r\n");
    expect(pty_read(kernel, p.shell, fds[1]) == "helo\n");
    // A partial line does not satisfy the slave; OPOST expands slave output.
    expect(pty_write(kernel, p.shell, fds[0], "wait") == 4 && pty_read(kernel, p.shell, fds[0]) == "wait");
    expect(pty_poll(kernel, p.shell, fds[1], OK_POLLIN) == 0);
    expect(pty_write(kernel, p.shell, fds[1], "a\nb") == 3 && pty_read(kernel, p.shell, fds[0]) == "a\r\nb");
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TCFLSH, 0) == 0);
    return true;
}

bool raw_drain(Kernel &kernel, const PtySession &p)
{
    // OK_SYS_PTY_PAIR keeps v1: the slave drains then reads EOF, writes EIO,
    // polls HUP|ERR with IN while data remains and never OUT.
    const auto fds = raw_pair(kernel, p.shell);
    expect(pty_write(kernel, p.shell, fds[0], "abc") == 3);
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(fds[0])}) == 0);
    expect(pty_poll(kernel, p.shell, fds[1], OK_POLLIN | OK_POLLOUT) == (OK_POLLIN | OK_POLLHUP | OK_POLLERR));
    expect(pty_read(kernel, p.shell, fds[1]) == "abc" && pty_read(kernel, p.shell, fds[1]).empty());
    expect(pty_write(kernel, p.shell, fds[1], "x") == -5);
    expect(fd_ioctl(kernel, p.shell, fds[1], OK_TCGETS, argument_at) == 0); // Not hung up.
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(fds[1])}) == 0);
    return true;
}

bool ptn_lock(Kernel &kernel, const PtySession &p, i32 &master, i32 &slave)
{
    auto &scheduler = kernel.scheduler();
    sched::private_pty::Handle handle{};
    master = bind_fd(kernel, p.shell, [&](u16 id) {
        const auto bound = scheduler.bind_pty_multiplexer(id);
        if (bound) handle = bound.value();
        return bound.ok();
    });
    expect(master >= 0);
    const sched::private_pty::Handle pts{handle.slot, handle.generation, sched::private_pty::Side::slave};
    expect(fd_ioctl(kernel, p.shell, master, OK_TIOCGPTN, argument_at) == 0 && int_at(argument_at) == handle.slot);
    // A ptmx master reports its slave's foreground without a check (Linux), 0 when none.
    expect(fd_ioctl(kernel, p.shell, master, OK_TIOCGPGRP, argument_at) == 0 && int_at(argument_at) == 0);
    expect(fd_ioctl(kernel, p.shell, master, OK_TIOCGSID, argument_at) == -25); // No session.
    expect(bind_fd(kernel, p.shell, [&](u16 id) { return scheduler.bind_pty_slave(id, pts).ok(); }) < 0); // Locked.
    // A dormant slave: the master's reads wait and its writes fail.
    expect(pty_write(kernel, p.shell, master, "x") == -5);
    expect(pty_poll(kernel, p.shell, master, OK_POLLIN) == 0);
    const i32 unlock = 0;
    std::memcpy(user(argument_at), &unlock, 4);
    expect(fd_ioctl(kernel, p.shell, master, OK_TIOCSPTLCK, argument_at) == 0);
    slave = bind_fd(kernel, p.shell, [&](u16 id) { return scheduler.bind_pty_slave(id, pts).ok(); });
    expect(slave >= 0);
    expect(fd_ioctl(kernel, p.shell, slave, OK_TIOCSPTLCK, argument_at) == -25);
    expect(fd_ioctl(kernel, p.shell, master, OK_TIOCSPTLCK, 0) == -14); // Copy-in after the checks.
    // ptmx pairs use the pts termios: cooked with echo.
    expect(pty_write(kernel, p.shell, master, "pt\r") == 3 && pty_read(kernel, p.shell, master) == "pt\r\n");
    expect(pty_read(kernel, p.shell, slave) == "pt\n");
    return true;
}

bool ptmx_hangup_discard(Kernel &kernel, const PtySession &p, i32 master, i32 slave)
{
    expect(pty_write(kernel, p.shell, master, "lost\n") == 5);
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(master)}) == 0);
    expect(pty_read(kernel, p.shell, slave).empty());
    expect(pty_write(kernel, p.shell, slave, "x") == -5);
    expect(fd_ioctl(kernel, p.shell, slave, OK_TCGETS, argument_at) == -5);
    expect(fd_ioctl(kernel, p.shell, slave, OK_TIOCSPGRP, argument_at) == -25);
    expect(pty_poll(kernel, p.shell, slave, OK_POLLIN | OK_POLLOUT) ==
           (OK_POLLIN | OK_POLLOUT | OK_POLLERR | OK_POLLHUP));
    expect(pty_call(kernel, p.shell, N::close, {static_cast<uptr>(slave)}) == 0);
    expect(kernel.scheduler().terminal_audit().ok());
    return true;
}

// Non-canonical console reads plan with VTIME taken as 0 and need
// min(VMIN, count) bytes, so readers wake on every buffered byte and after
// every termios change (n_tty __receive_buf and n_tty_set_termios).
bool vmin_wake(Kernel &kernel, const Session &s)
{
    const auto *reader = kernel.scheduler().find_thread(s.login, s.login);
    const auto blocked = [&] {
        return reader->state == sched::ProcessState::blocked &&
               reader->wait_reason == sched::ThreadWaitReason::console_read;
    };
    const auto block = [&](usize count) {
        bool pending = false;
        expect(native_call(kernel, s.login, N::read, {0, read_at, count}, &pending) == -11 && pending && blocked());
    };
    auto termios = tty::linux_std_termios();
    termios.lflag &= ~(tty::lflag::icanon | tty::lflag::echo);
    termios[tty::Cc::min] = 5;
    termios[tty::Cc::time] = 3;
    set_termios(kernel, s.login, termios);
    // VMIN 5, VTIME 3: each byte wakes the reader, which re-blocks until the fifth.
    for (const char value : std::string_view{"abcd"})
    {
        block(10);
        type({&value, 1});
        expect(!blocked());
    }
    block(10);
    type("e");
    expect(!blocked() && read_text(kernel, s.login, 10) == "abcde");
    // VMIN 5, VTIME 0: a read of 2 returns after the second byte.
    termios[tty::Cc::time] = 0;
    set_termios(kernel, s.login, termios);
    block(2);
    type("x");
    expect(!blocked());
    block(2);
    type("y");
    expect(!blocked() && read_text(kernel, s.login, 2) == "xy");
    // Lowering VMIN below the buffered count wakes the reader, although the
    // ldisc was already poll-readable under the stored VTIME.
    termios[tty::Cc::time] = 3;
    set_termios(kernel, s.login, termios);
    type("pqr");
    block(10);
    termios[tty::Cc::min] = 2;
    set_termios(kernel, s.job, termios);
    expect(!blocked() && read_text(kernel, s.login, 10) == "pqr");
    take_output();
    return true;
}
} // namespace

int main()
{
    static_cast<void>(&create); // Fixture helpers this suite does not need.
    static_cast<void>(&release);
    static Kernel kernel; CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto session = open_console(kernel);
    const bool compat = compat_default(kernel, session);
    const bool canonical = icanon_echo(kernel, session);
    const bool signals = isig(kernel, session);
    const bool window = winch(kernel, session);
    const bool flushed = flush(kernel, session);
    const bool large = writev_8k(kernel, session);
    const bool kept = efault_keeps_input(kernel, session);
    const bool vmin = vmin_wake(kernel, session);
    // Revocation resets the whole terminal, ldisc and window included.
    auto &console = kernel.scheduler().console_terminal();
    set_termios(kernel, session.login, tty::linux_std_termios());
    type("left");
    expect(native_call(kernel, session.init, N::console_revoke, {session.epoch}) == 0);
    expect(console.termios == tty::console_compat_termios() && !console.ldisc.buffered() && !console.session);
    PtySession pty{};
    std::array<i32, 2> fds{};
    i32 master = -1, slave = -1;
    const bool terminal = pty_terminal(kernel, session, pty, fds);
    const bool woken = pty_wakes(kernel, pty, fds);
    const bool echoed = canonical_echo(kernel, pty, fds);
    const bool drained = raw_drain(kernel, pty);
    const bool locked = ptn_lock(kernel, pty, master, slave);
    const bool discarded = ptmx_hangup_discard(kernel, pty, master, slave);
    expect(kernel.scheduler().pty_audit().live_pairs == 1 && kernel.scheduler().terminal_audit().ok());
    expect(native::runtime_status().ok());
    std::printf("NATIVE_TTY_RUNTIME_PASS console=1 compat_default=%d icanon=%d echo=%d isig=%d winch=%d flush=%d "
                "writev_8k=%d efault_keeps_input=%d vmin_wake=%d pty=%d canonical_echo=%d ptmx_hangup_discard=%d "
                "raw_drain=%d ptn_lock=%d hosted=1 guest=0\n",
                compat, canonical, canonical, signals, window, flushed, large, kept, vmin && woken, terminal, echoed, discarded,
                drained, locked);
}
