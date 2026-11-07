#pragma once

#include "ok/fs/terminal_node.hpp"

namespace ok::sched
{
// One open() of a terminal device node (docs/modules/tty.md section 3). The
// POSIX layer decodes the access mode and O_NOCTTY; `flags` is the status word
// the description keeps (F_GETFL), already without the open-only flags.
struct TerminalOpenRequest
{
    fs::TerminalNode node{};
    u32 flags{};
    bool readable{}, writable{}, no_controlling_terminal{};
    bool large_file{}; // Kept as on regular opens, so Linux F_GETFL reports O_LARGEFILE.
    u32 euid{}, egid{}; // Owner of a new /dev/pts entry.
};

struct TerminalOpenResult
{
    fs::TerminalNodeKind bound{}; // The node kind the description reports (its origin).
    u16 index{};                  // PTY slot, for /dev/ptmx and /dev/pts/N.
    bool acquired{};              // The open made the slave the caller's controlling terminal.
};

// What fstat reports for a terminal description: the node it was opened from,
// or the console for the boot stdio.
struct TerminalStatus
{
    fs::TerminalNodeKind kind{};
    fs::FilesystemDevice rdev{};
    u64 inode{};
    u32 uid{}, gid{}, mode{}; // mode: permission bits only.
};
} // namespace ok::sched
