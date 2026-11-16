# Bounded controlling console and foreground groups

The ABI73 / UAPI0.15 candidate adds an explicitly acquired controlling console
under the existing PID1-issued input lease. The mandatory SDK capability is
`native_console_control_profile=bootcpu-lease-epoch-explicit-session-foreground-eio-pid32-ioctl-v1`.
Session profile v3 and group-signal profile v2 describe the same bounded cohort.
This document defines implementation scope; execution qualification is separate.
Since WT2.2 the capability is
`native_console_control_profile=bootcpu-lease-epoch-explicit-console-pty-session-foreground-sigttin-sigttou-pid32-ioctl-v2`
(UAPI 0.17.0, session profile v4, group-signal profile v3), and the
[terminal contract](../modules/tty.md) describes current behaviour. Statements
below that WT2.2 replaced are marked as v1 history.

The scheduler owns an epoch, controlling SID and foreground PGID. Lease checks
and input ingress retain their independent [lease contract](NATIVE_CONSOLE_INPUT.md).
Before explicit attachment, a lease member can read authentication input.
`TIOCSCTTY` with scalar zero attaches only when the caller is both the original
lease leader and its current session leader. In v1, forced acquisition
returned EOPNOTSUPP; since WT2.2 the argument is not examined and a terminal
owned by another session gives EPERM either way. A repeated acquisition by
that leader succeeds without resetting the foreground group. In v1, ordinary
`/dev/console` or `/dev/tty0` opens conferred no authority: operations
required canonical inherited or duplicated console OFDs. Since WT2.2 the
kernel device filesystem has `/dev/console`, whose opens never acquire (reads
stay lease-gated), `/dev/tty` for the caller's controlling terminal, and
`/dev/pts/N`, whose open without `O_NOCTTY` can make a PTY slave the
controlling terminal of a session leader without one.

`TIOCGSID` and `TIOCGPGRP` require current lease membership and the controlling
SID. `TIOCSPGRP` copies a signed 32-bit PGID: negative values return EINVAL,
zero or absent groups return ESRCH, and groups in another session return EPERM.
Retained zombie membership reserves the group until reap. There is no Linux
PID-to-PGID fallback. In v1, same-session background callers could set the
foreground group without SIGTTOU. Since WT2.2 a background `TIOCSPGRP` sends
SIGTTOU to the caller's group unless SIGTTOU is ignored or blocked, and gives
ENOTTY in an orphaned group. Credential UID zero does not bypass association
checks, and ordinary users can restore their same-session shell group.

In v1, background or detached reads and readv returned EIO without consuming
input; poll reported ERR without HUP, select folded ERR into interested
read/write sets without exceptional data, and blocked readers rechecked
canonical SID, PGID and epoch after attachment, group transfer, setpgid or
setsid. Since WT2.2 that gate is gone: a background reader in the console's
session gets SIGTTIN (EIO only when SIGTTIN is ignored or blocked or its group
is orphaned), lease members in other sessions read without a check, and
readiness and blocked readers keep only the lease check. A revoked lease
retains EACCES / ERR|HUP behavior. Readiness completion uses the ordinary
retained description/runtime scan. Fork inherits association, exec preserves
it, and setsid logically detaches by SID mismatch. Revoke or
lease-leader/PID1 retirement clears attachment and input; since WT2.2 it also
sends SIGHUP then SIGCONT to the console session leader and resets the console
terminal. An empty foreground group is never replaced automatically.

The native ioctl requests are uniform across all ten profiles. Linux requests
come from checked Linux 6.13.4 and musl 1.2.5 source macros, including MIPS and
PowerPC high-bit encodings. Linux fd, command and acquisition scalar use their
low 32 bits; pointer width follows the target ABI. Every pointed PID uses
exactly four target-endian bytes, including LP64. Checked copies occur only at
the native/Linux user boundary; PosixService accepts scalar kernel-owned values.

ObfuscationOS authenticates, calls setsid, explicitly attaches and checks the
console, then drops credentials and directly executes its shell. The shell
retains a CLOEXEC console alias, forms each gated pipeline's first-child PGID,
hands foreground authority over before gate release, waits, and restores its
group before accepting another command. Reclaim failure stops the command list.
Redirected descriptors and noninteractive/no-console execution remain supported.

Hosted runtime tests use mocked hardware and checked-copy mappings; ten profile
adapters in one host executable do not represent ten native guests. Debug
`console-control / scalar-copy-wire-model` checks only scalar/copy marshalling.
SDK admission and four supplied musl compile checks prove artifact compatibility,
not target execution. Actual login, foreground input and pipeline campaigns
must bind their exact sources, SDK, compiler and serial observations separately.
This v1 slice made no PTY, termios/line discipline, generic `/dev/tty`
acquisition, terminal-generated HUP/stop/continue or complete job control
claim. Those landed in WT2.2 (hosted runtime evidence; guest lanes in the
paired OS worktree `feat/getty-login`), as the
[terminal contract](../modules/tty.md) describes. There is still no SMP,
console vhangup or physical execution claim. Complete M0-M12 gates remain open.
