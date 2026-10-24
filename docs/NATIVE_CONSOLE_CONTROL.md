# Bounded controlling console and foreground groups

The ABI73 / UAPI0.15 candidate adds an explicitly acquired controlling console
under the existing PID1-issued input lease. The mandatory SDK capability is
`native_console_control_profile=bootcpu-lease-epoch-explicit-session-foreground-eio-pid32-ioctl-v1`.
Session profile v3 and group-signal profile v2 describe the same bounded cohort.
This document defines implementation scope; execution qualification is separate.

The scheduler owns an epoch, controlling SID and foreground PGID. Lease checks
and input ingress retain their independent [lease contract](NATIVE_CONSOLE_INPUT.md).
Before explicit attachment, a lease member can read authentication input.
`TIOCSCTTY` with scalar zero attaches only when the caller is both the original
lease leader and its current session leader. Forced acquisition returns
EOPNOTSUPP. A repeated acquisition by that leader succeeds without resetting
the foreground group. Ordinary `/dev/console` or `/dev/tty0` opens confer no
authority: operations require canonical inherited or duplicated console OFDs.

`TIOCGSID` and `TIOCGPGRP` require current lease membership and the controlling
SID. `TIOCSPGRP` copies a signed 32-bit PGID: negative values return EINVAL,
zero or absent groups return ESRCH, and groups in another session return EPERM.
Retained zombie membership reserves the group until reap. There is no Linux
PID-to-PGID fallback. Same-session background callers may set the foreground
group in this profile; SIGTTOU behavior is not implemented. Credential UID zero
does not bypass association checks, and ordinary users can restore their
same-session shell group.

Background or detached reads and readv return EIO without consuming input.
Poll reports ERR without HUP; select folds ERR into interested read/write sets
without exceptional data. A revoked lease retains EACCES / ERR|HUP behavior.
Blocked readers recheck canonical SID, PGID and epoch after attachment, group
transfer, setpgid or setsid. Readiness completion uses the ordinary retained
description/runtime scan. Fork inherits association, exec preserves it, and
setsid logically detaches by SID mismatch. Revoke or lease-leader/PID1 retirement
clears attachment and input. An empty foreground group is never replaced
automatically.

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
There is no PTY, termios/line discipline, generic `/dev/tty` acquisition,
terminal-generated HUP/stop/continue, complete job control, SMP or physical
execution claim. Complete M0-M12 gates remain open.
