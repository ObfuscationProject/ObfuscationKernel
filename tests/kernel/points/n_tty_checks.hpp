#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_termios.hpp"
#include "ok/tty/n_tty.hpp"

#include <bit>

namespace ok::tests
{
// Value-layer models only: no terminal object, runtime path, trap or user
// execution exercises these headers yet.
namespace n_tty_detail
{
template <usize Size> std::span<const std::byte> bytes(const char (&text)[Size])
{
    return {reinterpret_cast<const std::byte *>(text), Size - 1};
}

template <class Tty> constexpr usize read_into(Tty &tty, const tty::Termios &termios, std::span<std::byte> out)
{
    const auto plan = tty.plan_read(termios, out.size(), true, false);
    const usize copied = tty.preview(plan, out);
    return tty.consume(plan) ? copied : ~usize{0};
}

constexpr bool canonical_constant()
{
    tty::NTty<64, 16> line{};
    const tty::Termios termios = tty::linux_std_termios();
    const std::byte input[]{std::byte{'o'}, std::byte{'k'}, std::byte{0x7f}, std::byte{'\r'}};
    const auto received = line.receive(termios, input, 1, false);
    const auto plan = line.plan_read(termios, 8, false, false);
    return received.became_readable && plan.copy == 2 && line.echo_pending() == 7; // "ok\b \b\r\n"
}
static_assert(canonical_constant());
} // namespace n_tty_detail

inline Status check_n_tty_line_discipline_model()
{
    using namespace n_tty_detail;
    tty::NTty<256, 32> line{};
    std::array<std::byte, 32> out{};
    const tty::Termios cooked = tty::linux_std_termios();
    auto received = line.receive(cooked, bytes("ab\x7f" "c\r"), 1, false);
    if (!received.became_readable || read_into(line, cooked, out) != 3 || out[0] != std::byte{'a'} ||
        out[1] != std::byte{'c'} || out[2] != std::byte{'\n'})
        return Status::fault("N_TTY canonical edit did not produce one line");
    const usize echoed = line.take_echo(out);
    if (echoed != 8 || out[2] != std::byte{'\b'} || out[6] != std::byte{'\r'} || out[7] != std::byte{'\n'})
        return Status::fault("N_TTY echo through OPOST differs");
    received = line.receive(cooked, bytes("zz\x03"), 2, false);
    if (received.signal_count != 1 || received.signals[0] != tty::TerminalSignal::interrupt ||
        !received.flush_peer_output || line.buffered() != 0)
        return Status::fault("N_TTY ISIG did not report and flush");
    line.take_echo(out);
    if (line.receive(cooked, bytes("\x04"), 1, false).consumed != 1 ||
        line.plan_read(cooked, 8, false, false).copy != 0 || read_into(line, cooked, out) != 0)
        return Status::fault("N_TTY EOF at line start must read 0");
    // Raw pairs: a FIFO with backpressure at the capacity and no echo.
    const tty::Termios raw = tty::raw_pair_termios();
    std::array<std::byte, 300> burst{};
    for (usize index = 0; index < burst.size(); ++index)
        burst[index] = static_cast<std::byte>(index);
    received = line.receive(raw, burst, 0, false);
    if (received.consumed != 256 || line.room(raw) != 0 || line.echo_pending() != 0 ||
        line.plan_read(raw, 300, false, false).copy != 256)
        return Status::fault("N_TTY raw pair is not a bounded FIFO");
    tty::Termios timed = raw;
    timed[tty::Cc::min] = 0;
    timed[tty::Cc::time] = 3;
    line.hangup();
    const auto plan = line.plan_read(timed, 8, false, false);
    if (plan.wait != tty::NTty<256, 32>::ReadWait::timeout || plan.timer_deciseconds != 3 ||
        !line.cleared_outside_live_bytes())
        return Status::fault("N_TTY VTIME plan or hangup wipe differs");
    return {};
}

// The generated Linux codec of the configured profile plus the native wire.
inline Status check_termios_codec_model()
{
    using namespace abi::linux_user;
    constexpr Profile profile = configured_native_profile();
    using Codec = TtyIoctlCodec<profile>;
    constexpr bool mips = profile == Profile::mips || profile == Profile::mips64;
    constexpr bool ppc = profile == Profile::ppc;
    static_assert(Codec::termios_bytes == (ppc ? 44 : mips ? 40 : 36));
    std::array<u8, 44> wire{};
    const std::span<u8> copy{wire.data(), Codec::termios_bytes};
    const tty::Termios standard = tty::linux_std_termios();
    tty::Termios back{};
    tty::ForeignResidue residue{};
    if (!Codec::encode_termios(standard, {}, copy) || !Codec::decode_termios(copy, back, residue) || back != standard)
        return Status::fault("configured Linux termios codec does not round-trip tty_std_termios");
    if (Codec::decode_termios({wire.data(), Codec::termios_bytes - 1}, back, residue))
        return Status::fault("configured Linux termios codec accepted a short copy");
    const u32 tcgets = mips ? 0x540dU : ppc ? 0x402c7413U : 0x5401U;
    const std::array<u64, 3> words{0, tcgets, 0};
    const auto decoded = Codec::decode(words);
    if (!decoded || decoded.value().operation != TerminalIoctl::get_attributes ||
        Codec::argument_bytes(TerminalIoctl::get_attributes) != Codec::termios_bytes)
        return Status::fault("configured Linux TCGETS request differs");
    using Native = tty::NativeTermiosWire<std::endian::native == std::endian::big>;
    std::array<u8, 36> native{};
    if (!Native::encode(standard, residue, native) || !Native::decode(native, back, residue) || back != standard ||
        residue != tty::ForeignResidue{})
        return Status::fault("native termios wire does not round-trip");
    return {};
}
} // namespace ok::tests
