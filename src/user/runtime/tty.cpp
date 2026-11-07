// Terminals in the runtime: console ingress with echo, console output through
// OPOST, prepared console reads (a Linux one binds its wait for signals), PTY
// VTIME expiry, terminal ioctls for the native personality, the job-control
// gate of every terminal call (SIGTTIN, SIGTTOU, TOSTOP; docs/modules/tty.md
// section 9.6), and the signal outboxes of the console and every PTY slave.
// The scheduler owns the terminal objects; this unit moves their bytes to the
// serial port and turns queued terminal signals into kernel-originated signals.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
// Bytes moved per copy: echo and output never need a buffer of the read size.
constexpr usize tty_chunk = 256;
// One console write or read call moves at most this much; a longer write is
// short, as a write to a terminal may be, and the caller writes the rest.
constexpr usize tty_console_write_limit = 64 * 1024;
constexpr usize tty_console_read_limit = sched::TerminalLdisc::read_capacity;

struct TtyEmitSink
{
    usize put(std::span<const std::byte> bytes)
    {
        emit({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        return bytes.size();
    }
};

void tty_emit_echo(sched::Terminal &terminal)
{
    TtyEmitSink sink;
    static_cast<void>(terminal.ldisc.drain_echo(sink));
}

[[maybe_unused]] u32 tty_native_signal(tty::TerminalSignal signal)
{
    switch (signal)
    {
    case tty::TerminalSignal::hup: return job::native.hup;
    case tty::TerminalSignal::interrupt: return job::native.interrupt;
    case tty::TerminalSignal::quit: return job::native.quit;
    case tty::TerminalSignal::suspend: return job::native.tstp;
    case tty::TerminalSignal::cont: return job::native.cont;
    case tty::TerminalSignal::ttin: return job::native.ttin;
    case tty::TerminalSignal::ttou: return job::native.ttou;
    case tty::TerminalSignal::winch: return job::native.window;
    }
    return 0;
}

// Ranges of the first `limit` bytes of `vectors`; the bytes they cover.
Result<usize> tty_user_ranges(const hw::AddressSpace &space, std::span<const PipeVector> vectors, usize limit,
                              bool write)
{
    usize total = 0;
    for (const auto &vector : vectors)
    {
        const usize length = vector.length < limit - total ? vector.length : limit - total;
        if (!hw::user_range(space, vector.base, length, write)) return Status::fault();
        total += length;
        if (total == limit) break;
    }
    return total;
}
} // namespace

TtyGate tty_check(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                  sched::TerminalRef ref, TtyAccess access)
{
    auto &scheduler = kernel.scheduler();
    // A park that ended (continue, foreground change) leaves its binding; it
    // goes before the call binds its next wait.
    if (process.resources().personality == Personality::linux)
        if (const auto origin = syscall_origins.identity(process, thread); origin.capture)
            syscall_origins.release_terminal(origin, thread.native_frame, true);
    const auto *terminal = scheduler.terminal(ref);
    if (!terminal || (access == TtyAccess::write && !(terminal->termios.lflag & tty::lflag::tostop))) return TtyGate::go;
    const auto facts = scheduler.terminal_job_facts(process.pid(), ref);
    if (!facts.is_ctty || !facts.has_foreground || facts.in_foreground) return TtyGate::go;
#ifdef OK_PRIVATE_POLL_MASK
    switch (private_terminal_background(kernel, process, thread, access == TtyAccess::read ? job::native.ttin : job::native.ttou))
    {
    case BackgroundAccess::allowed: return TtyGate::go;
    case BackgroundAccess::io_error: return TtyGate::io_error;
    case BackgroundAccess::restart: break;
    }
    return TtyGate::park;
#else
    return TtyGate::io_error;
#endif
}

Status tty_park(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                sched::TerminalRef ref, i32 fd, bool &parked)
{
#ifdef OK_PRIVATE_POLL_MASK
    bool complete = true;
    const auto result = private_job_control_park(kernel, process, thread, sched::SchedulerState::job_control_key(ref), fd,
                                                 complete);
    tty_drain_outbox(kernel);
    if (!complete)
    {
        parked = true;
        return Status::would_block("terminal call parked by job control");
    }
    return result == -12 ? Status::no_memory("job-control park") : Status::invalid_argument("job-control park");
#else
    static_cast<void>(kernel); static_cast<void>(process); static_cast<void>(thread); static_cast<void>(ref);
    static_cast<void>(fd); static_cast<void>(parked);
    return Status::io_error("terminal call in the background");
#endif
}

Status tty_gate(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                sched::TerminalRef ref, TtyAccess access, i32 fd, bool &parked)
{
    switch (tty_check(kernel, process, thread, ref, access))
    {
    case TtyGate::go: return {};
    case TtyGate::io_error: tty_drain_outbox(kernel); return Status::io_error("terminal call in the background");
    case TtyGate::park: break;
    }
    return tty_park(kernel, process, thread, ref, fd, parked);
}

bool tty_drain_outbox(Kernel &kernel)
{
    auto &scheduler = kernel.scheduler();
    bool drained = false;
    for (usize index = 0; index < sched::SchedulerState::terminal_count; ++index)
    {
        auto &terminal = scheduler.terminal_storage(index);
        if (!terminal.outbox_count) continue;
        drained = true;
#ifdef OK_PRIVATE_POLL_MASK
        // Each send may run exit and stop paths that queue again; stay bounded.
        for (usize sent = 0; terminal.outbox_count && sent < 4 * sched::terminal_outbox_capacity; ++sent)
        {
            const auto entry = terminal.take_outbox();
            const auto signal = tty_native_signal(entry.signal);
            if (entry.group) static_cast<void>(send_kernel_group_signal(kernel, entry.target, signal));
            else static_cast<void>(send_kernel_process_signal(kernel, entry.target, signal));
        }
#else
        while (terminal.outbox_count) static_cast<void>(terminal.take_outbox());
#endif
        // A master writer stopped at a full outbox (ISIG budget) may go on.
        if (index)
            scheduler.wake_waiters(sched::ThreadWaitReason::pty_write,
                sched::SchedulerState::pty_wait_key({static_cast<u16>(index - 1), 1, sched::private_pty::Side::master}));
    }
    return drained;
}

void tty_console_input(Kernel &kernel, char value, sched::WakeSource source)
{
    static_cast<void>(kernel.scheduler().console_input(value, source));
    tty_emit_echo(kernel.scheduler().console_terminal());
    tty_drain_outbox(kernel);
}

i64 tty_console_write(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &caller,
                      std::span<const PipeVector> vectors, i32 fd, bool &complete)
{
    auto &space = process.resources().native_space;
    const auto total = tty_user_ranges(space, vectors, tty_console_write_limit, false);
    if (!total) return -14;
    // n_tty_write under TOSTOP checks the writer at the start of the call.
    bool parked = false;
    if (auto status = tty_gate(kernel, process, caller, sched::console_terminal_ref, TtyAccess::write, fd, parked); !status)
    {
        if (parked) complete = false;
        return error(status);
    }
    auto &terminal = kernel.scheduler().console_terminal();
    // n_tty_write: pending echo goes out before program output.
    tty_emit_echo(terminal);
    std::array<std::byte, tty_chunk> input{};
    std::array<std::byte, 2 * tty_chunk> output{};
    usize written = 0;
    for (const auto &vector : vectors)
        for (usize position = 0; position < vector.length && written < total.value();)
        {
            const usize remaining = total.value() - written < vector.length - position
                ? total.value() - written : vector.length - position;
            const usize count = remaining < input.size() ? remaining : input.size();
            if (!hw::copy_from_user(space, input.data(), vector.base + position, count))
                return written ? static_cast<i64>(written) : i64{-14};
            std::span<const std::byte> pending{input.data(), count};
            while (!pending.empty())
            {
                // A whole expansion (at most 8 bytes, XTABS) always fits the output chunk.
                const auto moved = terminal.ldisc.process_output(terminal.termios, pending, output);
                emit({reinterpret_cast<const char *>(output.data()), moved.produced});
                pending = pending.subspan(moved.consumed);
            }
            position += count;
            written += count;
        }
    return static_cast<i64>(written);
}

i64 tty_console_read(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &caller,
                     const sched::OpenFileDescription &description, std::span<const PipeVector> vectors,
                     bool &complete, const std::array<uptr, 6> &arguments, bool vectored)
{
    if (!description.readable) return -9;
    auto &space = process.resources().native_space;
    const auto total = tty_user_ranges(space, vectors, tty_console_read_limit, true);
    if (!total) return -14;
    auto &scheduler = kernel.scheduler();
    // The job check runs at the first entry and again when a continue ended
    // the wait (Linux restarts the read after the stop interrupted it).
    if (!caller.console_wait_epoch || caller.terminal_gate_mark != process.job_control().continue_generation)
    {
        bool parked = false;
        if (auto status = tty_gate(kernel, process, caller, sched::console_terminal_ref, TtyAccess::read,
                                   static_cast<i32>(arguments[0]), parked); !status)
        {
            if (parked) complete = false;
            return error(status);
        }
    }
    const auto epoch = caller.console_wait_epoch ? caller.console_wait_epoch : process.console_epoch();
    const bool nonblocking = description.flags & posix::o_NONBLOCK;
    const auto plan = scheduler.prepare_console_read(process.pid(), epoch, total.value(), nonblocking);
    if (!plan)
    {
        if (plan.status().code() != StatusCode::would_block || nonblocking) return error(plan.status());
        caller.console_wait_epoch = epoch;
        caller.terminal_gate_mark = process.job_control().continue_generation;
        auto ticket = scheduler.register_current_wait(sched::ThreadWaitReason::console_read, epoch);
        if (!ticket)
        {
            caller.console_wait_epoch = 0;
            return error(ticket.status());
        }
        // A Linux read binds each wait of its re-dispatched call, so a caught
        // signal can interrupt or restart it (terminal signal family).
        if (process.resources().personality == Personality::linux)
            if (const auto origin = syscall_origins.identity(process, caller); origin.capture)
            {
                const SyscallOrigins::ConsoleReadBinding binding{ticket.value(), static_cast<i32>(arguments[0]),
                    arguments[1], arguments[2], epoch, vectored, total.value()};
                if (!syscall_origins.bind_console_read(origin, caller.native_frame, binding)) syscall_origins.revoke(caller);
            }
        complete = false;
        return -11;
    }
    // Copy out first and consume only after every copy worked: EFAULT keeps the input.
    std::array<std::byte, tty_chunk> chunk{};
    usize offset = 0;
    for (const auto &vector : vectors)
        for (usize position = 0; position < vector.length && offset < plan.value().copy;)
        {
            const auto count = scheduler.preview_console_read(plan.value(),
                std::span<std::byte>{chunk.data(), vector.length - position < chunk.size()
                    ? vector.length - position : chunk.size()}, offset);
            if (!hw::copy_to_user(space, vector.base + position, chunk.data(), count)) return -14;
            position += count;
            offset += count;
        }
    if (auto status = scheduler.commit_console_read(plan.value()); !status)
    {
        runtime_failure = status;
        return error(status);
    }
    return static_cast<i64>(plan.value().copy);
}

bool expire_tty_timers(Kernel &kernel, u64 now)
{
    bool pending = false;
    for (const auto &entry : pty_waits)
    {
        if (!entry.pid) continue;
        auto resolved = pty_wait_objects.resolve(entry.object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!wait.deadline || !thread || !thread->wait_registration.pending(wait.ticket)) continue;
        if (now < wait.deadline) { pending = true; continue; }
        // The read retries with the timer expired (resume_pty_io).
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::timeout));
    }
    return pending;
}

i64 tty_terminal_control(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<uptr, 6> &a,
                         bool &complete)
{
    auto &space = process.resources().native_space;
    using Codec = console_control::NativeTerminalCodec<std::endian::native == std::endian::big>;
    const auto fd = static_cast<i32>(a[0]);
    bool parked = false;
    const auto result = console_control::dispatch<Codec>(
        fd, console_control::native_operation(a[1]), a[2],
        [&](i32 descriptor) { return kernel.posix().terminal_descriptor(descriptor); },
        [&](const sched::TerminalSubject &subject, sched::ConsoleControlOperation operation) {
            if (auto status = kernel.posix().terminal_check(subject, operation); !status) return status;
            return terminal_request_gate(kernel, process.pid(), subject, operation, fd, parked);
        },
        [&](u64 address, std::span<u8> bytes) {
            return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size());
        },
        [&](const sched::TerminalSubject &subject, const sched::TerminalRequest &request) {
            return kernel.posix().terminal_control(subject, request);
        },
        [&](u64 address, usize bytes) { return hw::user_range(space, static_cast<uptr>(address), bytes, true); },
        [&](u64 address, std::span<const u8> bytes) {
            return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
        });
    tty_drain_outbox(kernel);
    if (parked) complete = false;
    return result;
}
} // namespace runtime_detail

// Linux tty_check_change callers: set_termios (TCSETS, TCSETSW, TCSETSF) and
// tiocspgrp check the slave a master names (real_tty); TCSBRK and TCFLSH check
// the descriptor's own terminal, so a master's are never checked.
Status terminal_request_gate(Kernel &kernel, sched::ProcessId pid, const sched::TerminalSubject &subject,
                             sched::ConsoleControlOperation operation, i32 fd, bool &parked)
{
    using Op = sched::ConsoleControlOperation;
    const bool checked = operation == Op::set_now || operation == Op::set_drain || operation == Op::set_flush ||
        operation == Op::set_foreground ||
        (subject.side != sched::TerminalSide::pty_master && (operation == Op::drain_break || operation == Op::flush));
    auto *process = kernel.scheduler().find(pid);
    auto *thread = kernel.scheduler().current_thread();
    if (!checked || !process || !thread || thread->owner != pid) return {};
    auto status = tty_gate(kernel, *process, *thread, subject.terminal, TtyAccess::change, fd, parked);
    // Linux tiocspgrp turns the gate's EIO into ENOTTY.
    if (!status && !parked && status.code() == StatusCode::io_error && operation == Op::set_foreground)
        return Status::not_terminal("background group cannot take the terminal");
    return status;
}

void terminal_request_done(Kernel &kernel)
{
    tty_drain_outbox(kernel);
}
} // namespace ok::user::native
