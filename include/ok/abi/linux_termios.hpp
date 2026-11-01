#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/tty/termios.hpp"

#include <array>
#include <optional>
#include <span>

// Linux termios and terminal ioctl codecs for one profile, driven only by the
// generated tables in linux_profile_data.hpp. Both copy exactly the bytes the
// Linux kernel copies (struct termios: 36, 40 or 44 bytes; winsize 8; int 4) and
// never follow a user pointer.
namespace ok::abi::linux_user
{

// Every terminal request in one order: the console controls in
// ConsoleIoctlOperation order, then the generated TtyIoctlOperation order.
enum class TerminalIoctl : u8
{
    acquire, get_session, get_foreground, set_foreground,
    get_attributes, set_now, set_drain, set_flush, drain_break, flush, get_window, set_window,
    detach, input_pending, get_pty_number, set_pty_lock, count
};
inline constexpr usize console_terminal_ioctls = static_cast<usize>(ConsoleIoctlOperation::count);
static_assert(console_terminal_ioctls + static_cast<usize>(TtyIoctlOperation::count) ==
              static_cast<usize>(TerminalIoctl::count));
static_assert(static_cast<usize>(TerminalIoctl::get_attributes) == console_terminal_ioctls &&
              static_cast<usize>(TerminalIoctl::set_pty_lock) - console_terminal_ioctls ==
                  static_cast<usize>(TtyIoctlOperation::set_pty_lock));

// What the third ioctl word carries. Pointer kinds name the copied object.
enum class TerminalArgument : u8 { none, scalar, termios_in, termios_out, window_in, window_out, int_in, int_out };

[[nodiscard]] constexpr TerminalArgument terminal_argument(TerminalIoctl operation)
{
    switch (operation)
    {
    case TerminalIoctl::acquire:
    case TerminalIoctl::drain_break:
    case TerminalIoctl::flush:
        return TerminalArgument::scalar;
    case TerminalIoctl::get_session:
    case TerminalIoctl::get_foreground:
    case TerminalIoctl::input_pending:
    case TerminalIoctl::get_pty_number:
        return TerminalArgument::int_out;
    case TerminalIoctl::set_foreground:
    case TerminalIoctl::set_pty_lock:
        return TerminalArgument::int_in;
    case TerminalIoctl::get_attributes:
        return TerminalArgument::termios_out;
    case TerminalIoctl::set_now:
    case TerminalIoctl::set_drain:
    case TerminalIoctl::set_flush:
        return TerminalArgument::termios_in;
    case TerminalIoctl::get_window:
        return TerminalArgument::window_out;
    case TerminalIoctl::set_window:
        return TerminalArgument::window_in;
    case TerminalIoctl::detach:
    case TerminalIoctl::count:
        break;
    }
    return TerminalArgument::none;
}

struct TerminalIoctlRequest
{
    i32 fd{};
    TerminalIoctl operation{};
    u64 argument{};
};

template <Profile Target>
    requires SupportedProfile<Target>
struct TermiosCodec final
{
    static constexpr const ProfileData &profile = Codec<Target>::profile;
    static constexpr TermiosLayout layout = profile.termios;
    static constexpr usize termios_bytes = layout.bytes;
    static constexpr bool big_endian = profile.big_endian;
    // A copy of this profile's rows only, so other profiles' tables stay out of the image.
    static constexpr std::array<TermiosMapRow, termios_map_rows> rows = termios_maps[static_cast<usize>(Target)];
    static constexpr std::array<u8, termios_named_slots> slots = profile.termios_slots;

    struct Unnamed
    {
        std::array<u8, 8> slots{};
        usize count{};
    };
    // Profile control slots without a generic name, ascending.
    static constexpr Unnamed unnamed = [] {
        Unnamed result{};
        for (usize slot = 0; slot < layout.nccs; ++slot)
        {
            bool named = false;
            for (const u8 used : slots)
                named = named || used == slot;
            if (!named)
                result.slots[result.count++] = static_cast<u8>(slot);
        }
        return result;
    }();
    static_assert(unnamed.count <= tty::ForeignResidue{}.cc.size());
    static_assert(layout.nccs >= termios_named_slots && layout.cc_offset + layout.nccs <= termios_bytes &&
                  layout.line_offset < termios_bytes && termios_bytes <= 64);
    static_assert(!layout.speed_offset || layout.speed_offset + 8 == termios_bytes);
    // Profile CIBAUD bits; a value there without a generic name stays in the residue.
    static constexpr u32 cibaud_profile_mask = [] {
        u32 mask = 0;
        for (const auto &row : rows)
            if (row.field == TermiosField::cflag && row.generic_mask == tty::cflag::cibaud)
                mask |= row.profile_mask;
        return mask;
    }();

    [[nodiscard]] static constexpr Status decode(std::span<const u8> in, tty::Termios &termios,
                                                 tty::ForeignResidue &residue)
    {
        if (in.size() != termios_bytes)
            return Status::fault("Linux termios copy must be exactly the kernel struct size");
        tty::Termios result{};
        tty::ForeignResidue foreign{};
        for (usize field = 0; field < 4; ++field)
        {
            const u32 word = tty::wire::load<big_endian, 4>(in, field * 4);
            u32 generic = 0, covered = 0;
            for (const auto &row : rows)
                if (static_cast<usize>(row.field) == field && (word & row.profile_mask) == row.profile_value)
                {
                    generic |= row.generic_value;
                    covered |= row.profile_mask;
                }
            word_of(result, field) = generic;
            foreign.flags[field] = word & ~covered;
        }
        result.line = in[layout.line_offset];
        for (usize slot = 0; slot < termios_named_slots; ++slot)
            result.cc[slot] = in[layout.cc_offset + slots[slot]];
        for (usize index = 0; index < unnamed.count; ++index)
            foreign.cc[index] = in[layout.cc_offset + unnamed.slots[index]];
        foreign.cc_count = static_cast<u8>(unnamed.count);
        // PPC speed words: Linux set_termios recomputes them from CBAUD/CIBAUD
        // and keeps the copied word only for a BOTHER rate.
        if constexpr (layout.speed_offset != 0)
        {
            if ((result.cflag & tty::cflag::cbaud) == tty::cflag::bother)
                foreign.speeds[1] = tty::wire::load<big_endian, 4>(in, layout.speed_offset + 4);
            if ((result.cflag & tty::cflag::cibaud) >> tty::cflag::ibshift == tty::cflag::bother)
                foreign.speeds[0] = tty::wire::load<big_endian, 4>(in, layout.speed_offset);
        }
        termios = result;
        residue = foreign;
        return Status::success();
    }

    [[nodiscard]] static constexpr Status encode(const tty::Termios &termios, const tty::ForeignResidue &residue,
                                                 std::span<u8> out)
    {
        if (out.size() != termios_bytes)
            return Status::fault("Linux termios copy must be exactly the kernel struct size");
        for (auto &byte : out)
            byte = 0;
        for (usize field = 0; field < 4; ++field)
        {
            const u32 generic = word_of(termios, field);
            u32 word = residue.flags[field];
            for (const auto &row : rows)
                if (static_cast<usize>(row.field) == field && (generic & row.generic_mask) == row.generic_value)
                    word |= row.profile_value;
            tty::wire::store<big_endian, 4>(out, field * 4, word);
        }
        out[layout.line_offset] = termios.line;
        for (usize slot = 0; slot < termios_named_slots; ++slot)
            out[layout.cc_offset + slots[slot]] = termios.cc[slot];
        for (usize index = 0; index < unnamed.count; ++index)
            out[layout.cc_offset + unnamed.slots[index]] = index < residue.cc_count ? residue.cc[index] : 0;
        if constexpr (layout.speed_offset != 0)
        {
            // tty_termios_baud_rate / tty_termios_input_baud_rate: CIBAUD B0 is the
            // output rate, BOTHER the copied word, an unnamed encoding 0.
            const u32 output =
                (termios.cflag & tty::cflag::cbaud) == tty::cflag::bother ? residue.speeds[1] : tty::output_baud(termios);
            const u32 input_bits = (termios.cflag & tty::cflag::cibaud) >> tty::cflag::ibshift;
            const u32 input = (residue.flags[2] & cibaud_profile_mask) ? 0
                              : input_bits == tty::cflag::bother  ? residue.speeds[0]
                              : input_bits                        ? tty::baud_rate(input_bits)
                                                                  : output;
            tty::wire::store<big_endian, 4>(out, layout.speed_offset, input);
            tty::wire::store<big_endian, 4>(out, layout.speed_offset + 4, output);
        }
        return Status::success();
    }

  private:
    [[nodiscard]] static constexpr u32 &word_of(tty::Termios &termios, usize field)
    {
        return field == 0 ? termios.iflag : field == 1 ? termios.oflag : field == 2 ? termios.cflag : termios.lflag;
    }
    [[nodiscard]] static constexpr u32 word_of(const tty::Termios &termios, usize field)
    {
        return field == 0 ? termios.iflag : field == 1 ? termios.oflag : field == 2 ? termios.cflag : termios.lflag;
    }
};

template <Profile Target>
    requires SupportedProfile<Target>
struct TtyIoctlCodec final
{
    using Termios = TermiosCodec<Target>;
    using Window = tty::WindowWire<Codec<Target>::profile.big_endian>;
    static constexpr usize termios_bytes = Termios::termios_bytes;
    static constexpr usize window_bytes = Window::bytes;
    static constexpr usize int_bytes = 4;

    [[nodiscard]] static constexpr std::optional<u32> number(TerminalIoctl operation)
    {
        const auto index = static_cast<usize>(operation);
        const auto &profile = Codec<Target>::profile;
        if (index < console_terminal_ioctls)
            return profile.console_ioctls[index];
        if (index < static_cast<usize>(TerminalIoctl::count))
            return profile.tty_ioctls[index - console_terminal_ioctls];
        return std::nullopt;
    }

    [[nodiscard]] static constexpr std::optional<TerminalIoctl> operation(u64 command)
    {
        // Linux sys_ioctl takes an unsigned int request even on 64-bit ABIs.
        const u32 request = static_cast<u32>(command);
        for (usize index = 0; index < static_cast<usize>(TerminalIoctl::count); ++index)
            if (number(static_cast<TerminalIoctl>(index)) == request)
                return static_cast<TerminalIoctl>(index);
        return std::nullopt;
    }

    [[nodiscard]] static constexpr usize argument_bytes(TerminalIoctl operation)
    {
        switch (terminal_argument(operation))
        {
        case TerminalArgument::termios_in:
        case TerminalArgument::termios_out:
            return termios_bytes;
        case TerminalArgument::window_in:
        case TerminalArgument::window_out:
            return window_bytes;
        case TerminalArgument::int_in:
        case TerminalArgument::int_out:
            return int_bytes;
        case TerminalArgument::none:
        case TerminalArgument::scalar:
            break;
        }
        return 0;
    }

    [[nodiscard]] static constexpr i32 scalar_argument(u64 argument)
    {
        return std::bit_cast<i32>(static_cast<u32>(argument));
    }

    [[nodiscard]] static Result<TerminalIoctlRequest> decode(std::span<const u64> arguments)
    {
        if (arguments.size() != 3)
            return Status::fault("Linux terminal ioctl argument frame is incomplete");
        const auto selected = operation(arguments[1]);
        if (!selected)
            return Status::not_terminal("Linux terminal ioctl request is outside the bounded profile");
        return TerminalIoctlRequest{scalar_argument(arguments[0]), *selected,
                                    arguments[2] & Codec<Target>::word_mask};
    }

    [[nodiscard]] static constexpr Status decode_termios(std::span<const u8> in, tty::Termios &termios,
                                                         tty::ForeignResidue &residue)
    {
        return Termios::decode(in, termios, residue);
    }
    [[nodiscard]] static constexpr Status encode_termios(const tty::Termios &termios,
                                                         const tty::ForeignResidue &residue, std::span<u8> out)
    {
        return Termios::encode(termios, residue, out);
    }
    [[nodiscard]] static constexpr Status decode_window(std::span<const u8> in, tty::WinSize &window)
    {
        return Window::decode(in, window);
    }
    [[nodiscard]] static constexpr Status encode_window(const tty::WinSize &window, std::span<u8> out)
    {
        return Window::encode(window, out);
    }
    [[nodiscard]] static constexpr Result<i32> decode_int(std::span<const u8> in)
    {
        if (in.size() != int_bytes)
            return Status::fault("Linux terminal ioctl int copy must be exactly four bytes");
        return std::bit_cast<i32>(tty::wire::load<Codec<Target>::profile.big_endian, 4>(in, 0));
    }
    [[nodiscard]] static constexpr Status encode_int(i32 value, std::span<u8> out)
    {
        if (out.size() != int_bytes)
            return Status::fault("Linux terminal ioctl int copy must be exactly four bytes");
        tty::wire::store<Codec<Target>::profile.big_endian, 4>(out, 0, std::bit_cast<u32>(value));
        return Status::success();
    }

    // Requests that encode a direction also encode the copied size; both must
    // match the argument kind (asm-generic: 2 size/14 bits at 30; MIPS/PPC: 3/13 at 29).
    [[nodiscard]] static consteval bool encoded_sizes_match()
    {
        constexpr bool wide = Target == Profile::mips || Target == Profile::mips64 || Target == Profile::ppc;
        constexpr u32 shift = wide ? 29 : 30, size_mask = wide ? 0x1fff : 0x3fff;
        constexpr u32 none = wide ? 1 : 0, read = 2, write = wide ? 4 : 1;
        for (usize index = 0; index < static_cast<usize>(TerminalIoctl::count); ++index)
        {
            const auto selected = static_cast<TerminalIoctl>(index);
            const u32 request = *number(selected), direction = request >> shift;
            const u32 size = (request >> 16) & size_mask;
            const auto kind = terminal_argument(selected);
            const bool reads = kind == TerminalArgument::termios_out || kind == TerminalArgument::window_out ||
                               kind == TerminalArgument::int_out;
            if (direction == read || direction == write)
            {
                if (reads != (direction == read) || size != argument_bytes(selected))
                    return false;
            }
            else if (direction == none && wide && (size || argument_bytes(selected)))
                return false;
        }
        return true;
    }
    static_assert(encoded_sizes_match());
    static_assert(window_bytes == 8);
};

namespace detail
{
// The generated asm-generic rows must equal the kernel-neutral constants.
[[nodiscard]] consteval bool generic_termios_matches()
{
    using N = TermiosName;
    const auto &rows = termios_maps[static_cast<usize>(Profile::x86_64)];
    const auto value = [&](N name) { return rows[static_cast<usize>(name)].generic_value; };
    const auto mask = [&](N name) { return rows[static_cast<usize>(name)].generic_mask; };
    const std::array<std::pair<N, u32>, 60> expected{{
        {N::ignbrk, tty::iflag::ignbrk}, {N::brkint, tty::iflag::brkint}, {N::ignpar, tty::iflag::ignpar},
        {N::parmrk, tty::iflag::parmrk}, {N::inpck, tty::iflag::inpck}, {N::istrip, tty::iflag::istrip},
        {N::inlcr, tty::iflag::inlcr}, {N::igncr, tty::iflag::igncr}, {N::icrnl, tty::iflag::icrnl},
        {N::iuclc, tty::iflag::iuclc}, {N::ixon, tty::iflag::ixon}, {N::ixany, tty::iflag::ixany},
        {N::ixoff, tty::iflag::ixoff}, {N::imaxbel, tty::iflag::imaxbel}, {N::iutf8, tty::iflag::iutf8},
        {N::opost, tty::oflag::opost}, {N::olcuc, tty::oflag::olcuc}, {N::onlcr, tty::oflag::onlcr},
        {N::ocrnl, tty::oflag::ocrnl}, {N::onocr, tty::oflag::onocr}, {N::onlret, tty::oflag::onlret},
        {N::ofill, tty::oflag::ofill}, {N::ofdel, tty::oflag::ofdel}, {N::tab3, tty::oflag::tab3},
        {N::cstopb, tty::cflag::cstopb}, {N::cread, tty::cflag::cread}, {N::parenb, tty::cflag::parenb},
        {N::parodd, tty::cflag::parodd}, {N::hupcl, tty::cflag::hupcl}, {N::clocal, tty::cflag::clocal},
        {N::addrb, tty::cflag::addrb}, {N::cmspar, tty::cflag::cmspar}, {N::crtscts, tty::cflag::crtscts},
        {N::b0, tty::cflag::b0}, {N::b38400, tty::cflag::b38400}, {N::b57600, tty::cflag::b57600},
        {N::b115200, tty::cflag::b115200}, {N::b4000000, tty::cflag::b4000000}, {N::bother, tty::cflag::bother},
        {N::cs5, tty::cflag::cs5}, {N::cs6, tty::cflag::cs6}, {N::cs7, tty::cflag::cs7}, {N::cs8, tty::cflag::cs8},
        {N::isig, tty::lflag::isig}, {N::icanon, tty::lflag::icanon}, {N::xcase, tty::lflag::xcase},
        {N::echo, tty::lflag::echo}, {N::echoe, tty::lflag::echoe}, {N::echok, tty::lflag::echok},
        {N::echonl, tty::lflag::echonl}, {N::noflsh, tty::lflag::noflsh}, {N::tostop, tty::lflag::tostop},
        {N::echoctl, tty::lflag::echoctl}, {N::echoprt, tty::lflag::echoprt}, {N::echoke, tty::lflag::echoke},
        {N::flusho, tty::lflag::flusho}, {N::pendin, tty::lflag::pendin}, {N::iexten, tty::lflag::iexten},
        {N::extproc, tty::lflag::extproc}, {N::cibaud_b4000000, tty::cflag::cibaud},
    }};
    for (const auto &[name, generic] : expected)
        if (value(name) != generic)
            return false;
    if (mask(N::nl1) != tty::oflag::nldly || mask(N::cr3) != tty::oflag::crdly || mask(N::tab3) != tty::oflag::tabdly ||
        mask(N::bs1) != tty::oflag::bsdly || mask(N::vt1) != tty::oflag::vtdly || mask(N::ff1) != tty::oflag::ffdly ||
        mask(N::b0) != tty::cflag::cbaud || mask(N::cs8) != tty::cflag::csize || mask(N::cibaud_b0) != tty::cflag::cibaud)
        return false;
    for (const auto &profile : profiles)
        if (profile.termios.nccs < termios_named_slots)
            return false;
    for (usize slot = 0; slot < termios_named_slots; ++slot)
        if (profiles[static_cast<usize>(Profile::x86_64)].termios_slots[slot] != slot)
            return false;
    for (const auto &row : rows)
        if (row.generic_mask != row.profile_mask || row.generic_value != row.profile_value)
            return false;
    return static_cast<usize>(tty::Cc::count) == termios_named_slots;
}
static_assert(generic_termios_matches());
} // namespace detail

} // namespace ok::abi::linux_user
