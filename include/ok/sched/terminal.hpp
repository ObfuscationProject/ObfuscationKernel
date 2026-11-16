#pragma once

#include "ok/core/types.hpp"
#include "ok/tty/n_tty.hpp"

#include <array>

// Terminal objects (docs/modules/tty.md section 1). The console is terminal 0;
// PTY slaves follow as 1 + slot. A terminal owns its termios, window, N_TTY
// line discipline, session association and a signal outbox. Scheduler code
// cannot call the runtime's signal senders, so it queues terminal signals in
// the outbox and the runtime drains it at the top of every step, after every
// terminal path and in the exit commit.
namespace ok::sched
{
using ProcessId = u64;
using TerminalLdisc = tty::NTty<4096, 512>;
inline constexpr usize terminal_outbox_capacity = 8;

enum class TerminalSide : u8 { console, pty_master, pty_slave };
// TIOCSCTTY (ioctl) or the automatic acquisition of an open without O_NOCTTY (S6).
enum class AcquireMode : u8 { ioctl, open };

struct TerminalRef
{
    u16 index{0xffff}; // 0 is the console; 1 + slot names a PTY slave.
    u64 generation{};  // Pair generation for a PTY slave; 0 for the console.
    [[nodiscard]] constexpr bool valid() const { return index != 0xffff; }
    constexpr bool operator==(const TerminalRef &) const = default;
};
inline constexpr TerminalRef console_terminal_ref{0, 0};

// A terminal open file description as an ioctl subject, keyed by the OFD.
struct TerminalSubject
{
    u16 description{0xffff};
    TerminalRef terminal{};
    TerminalSide side{TerminalSide::console};
};

// Inputs of the background-access decision (Linux __tty_check_change).
struct TerminalJobFacts
{
    bool is_ctty{}, has_foreground{}, in_foreground{};
    ProcessId pgrp{};
};

struct TerminalOutboxEntry
{
    ProcessId target{};
    tty::TerminalSignal signal{};
    bool group{}; // target names a process group rather than a process.
    constexpr bool operator==(const TerminalOutboxEntry &) const = default;
};

// The line discipline is a parameter only so that the private PTY owner model
// can run with small queues; every kernel terminal is a Terminal.
template <class Ldisc>
struct BasicTerminal
{
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    tty::WinSize window{};
    Ldisc ldisc{};
    ProcessId session{}, foreground{};
    u32 console_epoch{}; // Console only: the lease epoch the association belongs to.
    std::array<TerminalOutboxEntry, terminal_outbox_capacity> outbox{};
    u8 outbox_count{};
    bool hung_up{};

    // Fresh terminal state in place (no temporary of the ldisc size): the
    // given defaults, wiped input and echo, no association. The outbox is
    // kept; signals queued before a reset are still delivered.
    constexpr void reset(const tty::Termios &defaults, tty::WinSize size)
    {
        termios = defaults;
        residue = {};
        window = size;
        ldisc.hangup();
        session = foreground = 0;
        console_epoch = 0;
        hung_up = false;
    }

    [[nodiscard]] constexpr u8 outbox_room() const
    {
        return static_cast<u8>(terminal_outbox_capacity - outbox_count);
    }

    // Queues a signal once per (target, signal, group); false when full.
    constexpr bool owe(ProcessId target, tty::TerminalSignal signal, bool group)
    {
        if (!target) return true;
        const TerminalOutboxEntry entry{target, signal, group};
        for (usize index = 0; index < outbox_count; ++index)
            if (outbox[index] == entry) return true;
        if (outbox_count == terminal_outbox_capacity) return false;
        outbox[outbox_count++] = entry;
        return true;
    }

    // Removes and returns the oldest entry; the caller checks outbox_count.
    constexpr TerminalOutboxEntry take_outbox()
    {
        const auto entry = outbox[0];
        for (usize index = 1; index < outbox_count; ++index) outbox[index - 1] = outbox[index];
        outbox[--outbox_count] = {};
        return entry;
    }
};
using Terminal = BasicTerminal<TerminalLdisc>;

} // namespace ok::sched
