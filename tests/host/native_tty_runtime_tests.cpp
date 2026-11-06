// The console terminal in the actual runtime: N_TTY ingress, echo and OPOST
// output observed through the platform console callback, ISIG and SIGWINCH as
// kernel signals, flushes, large writev and prepared reads. Hosted trap and
// mapping backends; no guest execution.
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
    // Revocation resets the whole terminal, ldisc and window included.
    auto &console = kernel.scheduler().console_terminal();
    set_termios(kernel, session.login, tty::linux_std_termios());
    type("left");
    expect(native_call(kernel, session.init, N::console_revoke, {session.epoch}) == 0);
    expect(console.termios == tty::console_compat_termios() && !console.ldisc.buffered() && !console.session);
    expect(native::runtime_status().ok());
    std::printf("NATIVE_TTY_RUNTIME_PASS console=1 compat_default=%d icanon=%d echo=%d isig=%d winch=%d flush=%d "
                "writev_8k=%d efault_keeps_input=%d hosted=1 guest=0\n",
                compat, canonical, canonical, signals, window, flushed, large, kept);
}
