#pragma once

#include "ok/abi/linux_termios.hpp"
#include "ok/sched/console_control.hpp"
#include "ok/sched/terminal.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/tty/termios.hpp"
#include "ok/uapi/syscall.h"
#include <array>
#include <bit>
#include <concepts>
#include <optional>
#include <span>

// Terminal ioctl marshalling shared by both personalities (docs/modules/tty.md
// section 5). A codec copies exactly the bytes of one encoding: the native wire
// (36-byte termios, target endian) or a Linux profile (TtyIoctlCodec).
namespace ok::user::console_control
{
using Operation = sched::ConsoleControlOperation;
using Argument = abi::linux_user::TerminalArgument;

static_assert(static_cast<usize>(Operation::count) == static_cast<usize>(abi::linux_user::TerminalIoctl::count) &&
              static_cast<usize>(Operation::detach) == static_cast<usize>(abi::linux_user::TerminalIoctl::detach) &&
              static_cast<usize>(Operation::set_pty_lock) ==
                  static_cast<usize>(abi::linux_user::TerminalIoctl::set_pty_lock));

[[nodiscard]] constexpr Argument argument(Operation operation)
{
    return abi::linux_user::terminal_argument(static_cast<abi::linux_user::TerminalIoctl>(operation));
}

template <class Codec>
concept TerminalCodec = requires(std::span<const u8> in, std::span<u8> out, tty::Termios &termios,
                                 tty::ForeignResidue &residue, tty::WinSize &window) {
    { Codec::termios_bytes } -> std::convertible_to<usize>;
    { Codec::window_bytes } -> std::convertible_to<usize>;
    { Codec::int_bytes } -> std::convertible_to<usize>;
    { Codec::decode_termios(in, termios, residue) } -> std::same_as<Status>;
    { Codec::encode_termios(termios, residue, out) } -> std::same_as<Status>;
    { Codec::decode_window(in, window) } -> std::same_as<Status>;
    { Codec::encode_window(window, out) } -> std::same_as<Status>;
    { Codec::decode_int(in) } -> std::same_as<Result<i32>>;
    { Codec::encode_int(i32{}, out) } -> std::same_as<Status>;
};

// struct ok_termios, struct ok_winsize and int, target endian on every profile.
template <bool BigEndian>
struct NativeTerminalCodec final
{
    using Termios = tty::NativeTermiosWire<BigEndian>;
    using Window = tty::WindowWire<BigEndian>;
    static constexpr usize termios_bytes = Termios::termios_bytes;
    static constexpr usize window_bytes = Window::bytes;
    static constexpr usize int_bytes = 4;

    [[nodiscard]] static constexpr Status decode_termios(std::span<const u8> in, tty::Termios &termios,
                                                         tty::ForeignResidue &residue)
    { return Termios::decode(in, termios, residue); }
    [[nodiscard]] static constexpr Status encode_termios(const tty::Termios &termios,
                                                         const tty::ForeignResidue &residue, std::span<u8> out)
    { return Termios::encode(termios, residue, out); }
    [[nodiscard]] static constexpr Status decode_window(std::span<const u8> in, tty::WinSize &window)
    { return Window::decode(in, window); }
    [[nodiscard]] static constexpr Status encode_window(const tty::WinSize &window, std::span<u8> out)
    { return Window::encode(window, out); }
    [[nodiscard]] static constexpr Result<i32> decode_int(std::span<const u8> in)
    {
        if (in.size() != int_bytes) return Status::fault("terminal int copy must be exactly four bytes");
        return std::bit_cast<i32>(tty::wire::load<BigEndian, 4>(in, 0));
    }
    [[nodiscard]] static constexpr Status encode_int(i32 value, std::span<u8> out)
    {
        if (out.size() != int_bytes) return Status::fault("terminal int copy must be exactly four bytes");
        tty::wire::store<BigEndian, 4>(out, 0, std::bit_cast<u32>(value));
        return Status::success();
    }
};
static_assert(TerminalCodec<NativeTerminalCodec<false>> && TerminalCodec<NativeTerminalCodec<true>>);

[[nodiscard]] inline std::optional<Operation> native_operation(u64 request)
{
    switch (request)
    {
    case OK_TIOCSCTTY: return Operation::acquire;
    case OK_TIOCGSID: return Operation::get_session;
    case OK_TIOCGPGRP: return Operation::get_foreground;
    case OK_TIOCSPGRP: return Operation::set_foreground;
    case OK_TCGETS: return Operation::get_attributes;
    case OK_TCSETS: return Operation::set_now;
    case OK_TCSETSW: return Operation::set_drain;
    case OK_TCSETSF: return Operation::set_flush;
    case OK_TCSBRK: return Operation::drain_break;
    case OK_TCFLSH: return Operation::flush;
    case OK_TIOCGWINSZ: return Operation::get_window;
    case OK_TIOCSWINSZ: return Operation::set_window;
    case OK_FIONREAD: return Operation::input_pending;
    case OK_TIOCGPTN: return Operation::get_pty_number;
    case OK_TIOCSPTLCK: return Operation::set_pty_lock;
    default: return {};
    }
}

// Error order: describe (EBADF, ENOTTY for a non-terminal), unknown request
// (ENOTTY), association check (touches no user memory), copy-in (EFAULT),
// control (validation, then apply), copy-out (EOVERFLOW for a PID beyond
// INT32_MAX, then EFAULT). Callers bind the running canonical process first.
//   describe(fd) -> Result<sched::TerminalSubject>
//   check(subject, operation) -> Status
//   control(subject, const sched::TerminalRequest &) -> Result<sched::TerminalReply>
template <TerminalCodec Codec, typename Describe, typename Check, typename CopyIn, typename Control,
          typename Writable, typename CopyOut>
i64 dispatch(i32 fd, std::optional<Operation> operation, u64 address, Describe &&describe, Check &&check,
             CopyIn &&copy_in, Control &&control, Writable &&writable, CopyOut &&copy_out)
{
    const auto errno_of = [](Status status) { return -syscall::ErrnoMapper::errno_for(status); };
    const auto subject = describe(fd);
    if (!subject) return errno_of(subject.status());
    if (!operation || *operation >= Operation::count) return -25;
    if (const auto status = check(subject.value(), *operation); !status) return errno_of(status);
    sched::TerminalRequest request{};
    request.operation = *operation;
    const auto kind = argument(*operation);
    switch (kind)
    {
    case Argument::scalar:
        request.scalar = std::bit_cast<i32>(static_cast<u32>(address));
        break;
    case Argument::int_in:
    {
        std::array<u8, Codec::int_bytes> input{};
        if (!copy_in(address, std::span<u8>{input})) return -14;
        const auto value = Codec::decode_int(input);
        if (!value) return -14;
        request.scalar = value.value();
        break;
    }
    case Argument::termios_in:
    {
        std::array<u8, Codec::termios_bytes> input{};
        if (!copy_in(address, std::span<u8>{input}) ||
            !Codec::decode_termios(input, request.termios, request.residue))
            return -14;
        break;
    }
    case Argument::window_in:
    {
        std::array<u8, Codec::window_bytes> input{};
        if (!copy_in(address, std::span<u8>{input}) || !Codec::decode_window(input, request.window)) return -14;
        break;
    }
    default:
        break;
    }
    const auto result = control(subject.value(), static_cast<const sched::TerminalRequest &>(request));
    if (!result) return errno_of(result.status());
    const auto &reply = result.value();
    const auto publish = [&](std::span<const u8> bytes) -> i64 {
        if (!writable(address, bytes.size())) return -14;
        return copy_out(address, bytes) ? i64{0} : i64{-14};
    };
    switch (kind)
    {
    case Argument::int_out:
    {
        if (reply.value < 0 || reply.value > 0x7fffffff) return -75;
        std::array<u8, Codec::int_bytes> output{};
        if (!Codec::encode_int(static_cast<i32>(reply.value), output)) return -14;
        return publish(output);
    }
    case Argument::termios_out:
    {
        std::array<u8, Codec::termios_bytes> output{};
        if (!Codec::encode_termios(reply.termios, reply.residue, output)) return -14;
        return publish(output);
    }
    case Argument::window_out:
    {
        std::array<u8, Codec::window_bytes> output{};
        if (!Codec::encode_window(reply.window, output)) return -14;
        return publish(output);
    }
    default:
        return reply.value;
    }
}
} // namespace ok::user::console_control
