#pragma once

#include "ok/core/types.hpp"
#include "ok/tty/termios.hpp"

namespace ok::sched
{
// Terminal control operations (docs/modules/tty.md section 5). The first four
// keep their original values; the order equals abi::linux_user::TerminalIoctl.
enum class ConsoleControlOperation : u8
{
    acquire,
    get_session,
    get_foreground,
    set_foreground,
    get_attributes, // TCGETS
    set_now,        // TCSETS
    set_drain,      // TCSETSW
    set_flush,      // TCSETSF
    drain_break,    // TCSBRK
    flush,          // TCFLSH
    get_window,     // TIOCGWINSZ
    set_window,     // TIOCSWINSZ
    detach,         // TIOCNOTTY (S8)
    input_pending,  // FIONREAD
    get_pty_number, // TIOCGPTN (S5)
    set_pty_lock,   // TIOCSPTLCK (S5)
    count,
};

// One decoded request: `scalar` carries the scalar or the copied int (PID,
// queue selector, lock flag); termios and window carry copied structures.
struct TerminalRequest
{
    ConsoleControlOperation operation{};
    i32 scalar{};
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    tty::WinSize window{};
};

struct TerminalReply
{
    i64 value{};
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    tty::WinSize window{};
};
} // namespace ok::sched
