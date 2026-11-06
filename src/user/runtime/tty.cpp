// Terminals in the runtime: console ingress with echo, console output through
// OPOST, prepared console reads, terminal ioctls for the native personality,
// and the terminal signal outbox (docs/modules/tty.md). The scheduler owns the
// terminal objects; this unit moves their bytes to the serial port and turns
// queued terminal signals into kernel-originated signals.
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

void tty_drain_outbox(Kernel &kernel)
{
    auto &terminal = kernel.scheduler().console_terminal();
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
}

void tty_console_input(Kernel &kernel, char value, sched::WakeSource source)
{
    static_cast<void>(kernel.scheduler().console_input(value, source));
    tty_emit_echo(kernel.scheduler().console_terminal());
    tty_drain_outbox(kernel);
}

i64 tty_console_write(Kernel &kernel, sched::ProcessControlBlock &process, std::span<const PipeVector> vectors)
{
    auto &space = process.resources().native_space;
    const auto total = tty_user_ranges(space, vectors, tty_console_write_limit, false);
    if (!total) return -14;
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
                     bool &complete)
{
    if (!description.readable) return -9;
    auto &space = process.resources().native_space;
    const auto total = tty_user_ranges(space, vectors, tty_console_read_limit, true);
    if (!total) return -14;
    auto &scheduler = kernel.scheduler();
    const auto epoch = caller.console_wait_epoch ? caller.console_wait_epoch : process.console_epoch();
    const bool nonblocking = description.flags & posix::o_NONBLOCK;
    const auto plan = scheduler.prepare_console_read(process.pid(), epoch, total.value(), nonblocking);
    if (!plan)
    {
        if (plan.status().code() != StatusCode::would_block || nonblocking) return error(plan.status());
        caller.console_wait_epoch = epoch;
        if (auto status = scheduler.block_current(sched::ThreadWaitReason::console_read, epoch); !status)
        {
            caller.console_wait_epoch = 0;
            return error(status);
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

i64 tty_terminal_control(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<uptr, 6> &a)
{
    auto &space = process.resources().native_space;
    using Codec = console_control::NativeTerminalCodec<std::endian::native == std::endian::big>;
    const auto result = console_control::dispatch<Codec>(
        static_cast<i32>(a[0]), console_control::native_operation(a[1]), a[2],
        [&](i32 fd) { return kernel.posix().terminal_descriptor(fd); },
        [&](const sched::TerminalSubject &subject, sched::ConsoleControlOperation operation) {
            return kernel.posix().terminal_check(subject, operation);
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
    return result;
}
} // namespace runtime_detail
} // namespace ok::user::native
