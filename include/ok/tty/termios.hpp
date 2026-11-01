#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <concepts>
#include <span>

// Kernel-neutral termios. Every terminal stores its attributes in the asm-generic
// Linux encoding, which is also the native UAPI encoding on all ten profiles
// (ok/uapi/termios.h). Per-profile Linux encodings are translated by
// ok/abi/linux_termios.hpp; bits and slots without a generic name travel in a
// ForeignResidue so that they round-trip through Linux TCGETS/TCSETS.
namespace ok::tty
{

inline constexpr usize nccs = 19;

// Control character slots, asm-generic indices.
enum class Cc : u8
{
    intr, quit, erase, kill, eof, time, min, swtc, start, stop, susp,
    eol, reprint, discard, werase, lnext, eol2, count
};

namespace iflag
{
inline constexpr u32 ignbrk = 0x1, brkint = 0x2, ignpar = 0x4, parmrk = 0x8, inpck = 0x10, istrip = 0x20,
                     inlcr = 0x40, igncr = 0x80, icrnl = 0x100, iuclc = 0x200, ixon = 0x400, ixany = 0x800,
                     ixoff = 0x1000, imaxbel = 0x2000, iutf8 = 0x4000;
}
namespace oflag
{
inline constexpr u32 opost = 0x1, olcuc = 0x2, onlcr = 0x4, ocrnl = 0x8, onocr = 0x10, onlret = 0x20,
                     ofill = 0x40, ofdel = 0x80, nldly = 0x100, crdly = 0x600, tabdly = 0x1800, tab3 = 0x1800,
                     xtabs = 0x1800, bsdly = 0x2000, vtdly = 0x4000, ffdly = 0x8000;
}
namespace cflag
{
inline constexpr u32 cbaud = 0x100f, cbaudex = 0x1000, bother = 0x1000, b0 = 0x0, b38400 = 0xf,
                     b57600 = 0x1001, b115200 = 0x1002, b4000000 = 0x100f, csize = 0x30, cs5 = 0x0,
                     cs6 = 0x10, cs7 = 0x20, cs8 = 0x30, cstopb = 0x40, cread = 0x80, parenb = 0x100,
                     parodd = 0x200, hupcl = 0x400, clocal = 0x800, cibaud = 0x100f0000, addrb = 0x20000000,
                     cmspar = 0x40000000, crtscts = 0x80000000;
inline constexpr u32 ibshift = 16;
}
namespace lflag
{
inline constexpr u32 isig = 0x1, icanon = 0x2, xcase = 0x4, echo = 0x8, echoe = 0x10, echok = 0x20,
                     echonl = 0x40, noflsh = 0x80, tostop = 0x100, echoctl = 0x200, echoprt = 0x400,
                     echoke = 0x800, flusho = 0x1000, pendin = 0x4000, iexten = 0x8000, extproc = 0x10000;
}

// tcflush() queue selectors (TCFLSH argument).
enum class FlushQueue : u8 { input = 0, output = 1, both = 2 };

struct Termios
{
    u32 iflag{}, oflag{}, cflag{}, lflag{};
    u8 line{};
    std::array<u8, nccs> cc{};

    [[nodiscard]] constexpr u8 operator[](Cc slot) const { return cc[static_cast<usize>(slot)]; }
    [[nodiscard]] constexpr u8 &operator[](Cc slot) { return cc[static_cast<usize>(slot)]; }
    constexpr bool operator==(const Termios &) const = default;
};

struct WinSize
{
    u16 rows{}, cols{}, xpixel{}, ypixel{};
    constexpr bool operator==(const WinSize &) const = default;
};

// Profile-encoded flag bits and control slots that have no generic name, kept
// verbatim from the last Linux TCSETS* so a later TCGETS returns them.
struct ForeignResidue
{
    std::array<u32, 4> flags{}; // iflag, oflag, cflag, lflag
    std::array<u8, 8> cc{};      // unnamed slots in ascending slot order
    u8 cc_count{};
    constexpr bool operator==(const ForeignResidue &) const = default;
};

// Signals a terminal asks its owner to send; the line discipline never sends.
enum class TerminalSignal : u8 { hup, interrupt, quit, suspend, cont, ttin, ttou, winch };

// ^C ^\ DEL ^U ^D, VTIME 0, VMIN 1, VSWTC 0, ^Q ^S ^Z, VEOL 0, ^R ^O ^W ^V, VEOL2 0 (tty_std_termios).
[[nodiscard]] constexpr std::array<u8, nccs> standard_cc()
{
    return {0x03, 0x1c, 0x7f, 0x15, 0x04, 0x00, 0x01, 0x00, 0x11, 0x13, 0x1a,
            0x00, 0x12, 0x0f, 0x17, 0x16, 0x00, 0x00, 0x00};
}

// Linux tty_std_termios: the default of /dev/ptmx pairs.
[[nodiscard]] constexpr Termios linux_std_termios()
{
    return {iflag::icrnl | iflag::ixon, oflag::opost | oflag::onlcr,
            cflag::b38400 | cflag::cs8 | cflag::cread | cflag::hupcl,
            lflag::isig | lflag::icanon | lflag::echo | lflag::echoe | lflag::echok | lflag::echoctl |
                lflag::echoke | lflag::iexten,
            0, standard_cc()};
}

// musl/glibc cfmakeraw.
[[nodiscard]] constexpr Termios cfmakeraw(Termios termios)
{
    termios.iflag &= ~(iflag::ignbrk | iflag::brkint | iflag::parmrk | iflag::istrip | iflag::inlcr |
                       iflag::igncr | iflag::icrnl | iflag::ixon);
    termios.oflag &= ~oflag::opost;
    termios.lflag &= ~(lflag::echo | lflag::echonl | lflag::icanon | lflag::isig | lflag::iexten);
    termios.cflag = (termios.cflag & ~(cflag::csize | cflag::parenb)) | cflag::cs8;
    termios[Cc::min] = 1;
    termios[Cc::time] = 0;
    return termios;
}

// OK_SYS_PTY_PAIR raw pairs: with it N_TTY is byte-identical to a 4096-byte FIFO.
[[nodiscard]] constexpr Termios raw_pair_termios() { return cfmakeraw(linux_std_termios()); }

// Serial console default: CR becomes LF, no echo, no ONLCR, byte at a time. The
// kernel keeps it; a getty switches the console to cooked mode itself.
[[nodiscard]] constexpr Termios console_compat_termios()
{
    return {iflag::icrnl, 0, cflag::b115200 | cflag::cs8 | cflag::cread | cflag::clocal, 0, 0, standard_cc()};
}

[[nodiscard]] constexpr WinSize console_window() { return {24, 80, 0, 0}; }

// Numeric rate of a CBAUD value; 0 for B0, BOTHER and unknown encodings.
[[nodiscard]] constexpr u32 baud_rate(u32 cbaud_value)
{
    constexpr std::array<u32, 16> low{0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600,
                                      19200, 38400};
    constexpr std::array<u32, 16> high{0, 57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000,
                                       1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};
    if (cbaud_value & ~cflag::cbaud)
        return 0;
    return cbaud_value & cflag::cbaudex ? high[cbaud_value & 0xf] : low[cbaud_value];
}

[[nodiscard]] constexpr u32 output_baud(const Termios &termios) { return baud_rate(termios.cflag & cflag::cbaud); }

// CIBAUD B0 means "same as the output rate", as in tty_termios_input_baud_rate.
[[nodiscard]] constexpr u32 input_baud(const Termios &termios)
{
    const u32 input = (termios.cflag & cflag::cibaud) >> cflag::ibshift;
    return input ? baud_rate(input) : output_baud(termios);
}

namespace wire
{
template <bool BigEndian, usize Bytes>
[[nodiscard]] constexpr u32 load(std::span<const u8> bytes, usize offset)
{
    u32 value{};
    for (usize index = 0; index < Bytes; ++index)
        value |= static_cast<u32>(bytes[offset + index]) << (8 * (BigEndian ? Bytes - 1 - index : index));
    return value;
}
template <bool BigEndian, usize Bytes>
constexpr void store(std::span<u8> bytes, usize offset, u32 value)
{
    for (usize index = 0; index < Bytes; ++index)
        bytes[offset + index] = static_cast<u8>(value >> (8 * (BigEndian ? Bytes - 1 - index : index)));
}
} // namespace wire

// A termios wire turns exactly `termios_bytes` copied bytes into a Termios plus
// residue and back. Copies of any other size are refused untouched.
template <class Wire>
concept TermiosWire = requires(std::span<const u8> in, std::span<u8> out, Termios &termios,
                               const Termios &value, ForeignResidue &residue, const ForeignResidue &kept) {
    { Wire::termios_bytes } -> std::convertible_to<usize>;
    { Wire::big_endian } -> std::convertible_to<bool>;
    { Wire::decode(in, termios, residue) } -> std::same_as<Status>;
    { Wire::encode(value, kept, out) } -> std::same_as<Status>;
};

// struct ok_termios: four target-endian u32 flag words, c_line, c_cc[19]; 36 bytes.
template <bool BigEndian>
struct NativeTermiosWire final
{
    static constexpr usize termios_bytes = 36;
    static constexpr bool big_endian = BigEndian;

    [[nodiscard]] static constexpr Status decode(std::span<const u8> in, Termios &termios, ForeignResidue &residue)
    {
        if (in.size() != termios_bytes)
            return Status::fault("native termios copy must be exactly 36 bytes");
        Termios result{wire::load<BigEndian, 4>(in, 0), wire::load<BigEndian, 4>(in, 4),
                       wire::load<BigEndian, 4>(in, 8), wire::load<BigEndian, 4>(in, 12), in[16], {}};
        for (usize slot = 0; slot < nccs; ++slot)
            result.cc[slot] = in[17 + slot];
        termios = result;
        residue = {}; // The native personality has no foreign encoding.
        return Status::success();
    }

    [[nodiscard]] static constexpr Status encode(const Termios &termios, const ForeignResidue &, std::span<u8> out)
    {
        if (out.size() != termios_bytes)
            return Status::fault("native termios copy must be exactly 36 bytes");
        wire::store<BigEndian, 4>(out, 0, termios.iflag);
        wire::store<BigEndian, 4>(out, 4, termios.oflag);
        wire::store<BigEndian, 4>(out, 8, termios.cflag);
        wire::store<BigEndian, 4>(out, 12, termios.lflag);
        out[16] = termios.line;
        for (usize slot = 0; slot < nccs; ++slot)
            out[17 + slot] = termios.cc[slot];
        return Status::success();
    }
};

// struct winsize / struct ok_winsize: four target-endian u16, identical on every profile.
template <bool BigEndian>
struct WindowWire final
{
    static constexpr usize bytes = 8;

    [[nodiscard]] static constexpr Status decode(std::span<const u8> in, WinSize &window)
    {
        if (in.size() != bytes)
            return Status::fault("winsize copy must be exactly 8 bytes");
        window = {static_cast<u16>(wire::load<BigEndian, 2>(in, 0)), static_cast<u16>(wire::load<BigEndian, 2>(in, 2)),
                  static_cast<u16>(wire::load<BigEndian, 2>(in, 4)), static_cast<u16>(wire::load<BigEndian, 2>(in, 6))};
        return Status::success();
    }

    [[nodiscard]] static constexpr Status encode(const WinSize &window, std::span<u8> out)
    {
        if (out.size() != bytes)
            return Status::fault("winsize copy must be exactly 8 bytes");
        wire::store<BigEndian, 2>(out, 0, window.rows);
        wire::store<BigEndian, 2>(out, 2, window.cols);
        wire::store<BigEndian, 2>(out, 4, window.xpixel);
        wire::store<BigEndian, 2>(out, 6, window.ypixel);
        return Status::success();
    }
};

static_assert(TermiosWire<NativeTermiosWire<false>> && TermiosWire<NativeTermiosWire<true>>);
static_assert(cfmakeraw(linux_std_termios()).lflag == (lflag::echoe | lflag::echok | lflag::echoctl | lflag::echoke));

} // namespace ok::tty
