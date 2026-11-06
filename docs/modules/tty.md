# Terminals and job control

This is the terminal and job-control contract of WT2.2 `feat/tty-jobctl`
(`docs/PLAN.md`). The paired ObfuscationOS worktree `feat/getty-login` builds
against it. Every section carries a status: **planned (Sn)** names the slice
that lands it, and becomes **landed (Sn)** when that slice merges into this
branch. Nothing below is implemented yet unless it is marked landed.

Current behaviour, which the remaining planned sections replace:

- The serial console is a lease (`OK_SYS_CONSOLE_CLAIM/REVOKE/FLUSH`) over a
  terminal object (S4): an N_TTY line discipline under the raw-compatible
  `console_compat` termios (CR becomes LF through ICRNL, no echo, no ONLCR,
  byte at a time). The console serves TCGETS, TCSETS/TCSETSW/TCSETSF, TCSBRK,
  TCFLSH, TIOCGWINSZ, TIOCSWINSZ and FIONREAD besides `OK_TIOCSCTTY`,
  `OK_TIOCGPGRP`, `OK_TIOCSPGRP` and `OK_TIOCGSID`, for both personalities, so
  musl `isatty()` on a console descriptor is 1. Background console reads keep
  returning EIO until S8.
- `OK_SYS_PTY_PAIR` creates raw native PTY pairs (8 slots) with the v1 hangup
  rules of §8. Every PTY slave is a terminal with N_TTY under raw termios, so
  raw pairs move bytes exactly as the former 4096-byte queues (S5). PTY
  descriptors serve the terminal requests of §5 in both personalities; Linux
  PTY read, write and poll still return EOPNOTSUPP until S7. ptmx-style pairs
  (§8) exist behind the scheduler bind calls and become reachable through
  `/dev/ptmx` in S6.
- Job control (§9.1-§9.5, §9.8 and the orphan row of §9.7) is landed: every
  signal number reaches native and Linux recipients, stops and continues are
  reported through WUNTRACED/WCONTINUED and SIGCHLD, and native programs have
  SIG_DFL/SIG_IGN. On the console, ISIG characters and TIOCSWINSZ generate
  signals (S4), and so do PTY master input and TIOCSWINSZ on a PTY (S5);
  gates and hangup signals follow in S8.
- `OK_TTY` (`fs::TtyDevice`) is a VFS roadmap model. It is not a terminal and is
  not part of this contract.

Scope. All mechanisms run on the boot CPU, serialized at the runtime `step()`
boundary. Hosted model and runtime runners are model/hosted evidence; debug
test points are models booted in QEMU; only the ObfuscationOS guest probes
(`TTY_DEVPTS_*`, the login and oksh lanes) are guest execution. The contract
makes no SMP, physical-hardware or process-isolation claim.

UAPI placement:

| Header | Content | Slice |
|---|---|---|
| `ok/uapi/termios.h` (new) | `struct ok_termios`, `struct ok_winsize`, flag and cc macros, `OK_TCIFLUSH/OK_TCOFLUSH/OK_TCIOFLUSH` | landed (S1) |
| `ok/uapi/syscall.h` (additive defines only) | `OK_TCGETS` … `OK_TIOCNOTTY` (S1), `OK_TIOCGPTN`, `OK_TIOCSPTLCK` (S5), `OK_O_NOCTTY`, `OK_EINTR`, `OK_ENXIO`, `OK_ENOSPC`, `OK_PTS_MAJOR`, `OK_PTS_CAPACITY` (S6) | landed (S1); planned (S5, S6) |
| `ok/uapi/job_control.h` (new; C, include guard only) | native signal numbers other than KILL and TERM, `OK_SIG_DFL`, `OK_SIG_IGN`, `struct ok_sigaction`, `OK_NATIVE_SIGNAL_ACTION_VERSION`, `OK_WNOHANG`, `OK_WUNTRACED`, `OK_WCONTINUED` | S3 |
| `ok/uapi/signal.h` | unchanged until S9; S9 decides whether `job_control.h` folds into it with UAPI 0.17 | S9 |

Existing lines of `syscall.h` and `signal.h`, the profile marker headers, module
ABI 78 and UAPI 0.16.0 stay unchanged until S9, so the unmodified ObfuscationOS
consumer accepts every intermediate SDK.

## 1. Terminal objects — landed (S4 console, S5 PTY)

There are nine terminal objects: the serial console and one per PTY slave (8
slots). Each owns termios (stored in the asm-generic encoding plus a
per-profile residue), a winsize, an N_TTY line discipline, a session and
foreground process group, and a hung-up flag. A PTY master is not a terminal of
its own: termios and window operations on a master act on its slave.

Landed (S4): `ok/sched/terminal.hpp` defines `Terminal` (termios, residue,
window, `NTty<4096, 512>` ldisc, session, foreground, console lease epoch, an
8-entry signal outbox and the hung-up flag), `TerminalRef` (0 = console, 1 +
slot = PTY slave), `TerminalSide`, `TerminalSubject` (keyed by the open file
description) and `TerminalJobFacts`. The scheduler owns the console terminal
and queues terminal signals in its outbox; the runtime drains the outbox at
the top of every step, after every terminal path and in the exit commit
(before the orphaned-group signals). Lease claim, revoke and retirement reset
the whole console terminal: compat termios, wiped ldisc, 24×80 window, no
association.

Landed (S5): each slot of the private PTY table (`raw_pty_transport.hpp`)
embeds its slave `Terminal`, which replaces the former slave queue; pair
creation resets it with the pair's default termios (§6) and a 0×0 window.
Master writes run `NTty::receive` on the slave (echo into the master queue,
ISIG into the slave terminal's outbox), slave writes run `process_output`
into the master queue, and slave reads are planned, copied out and then
consumed. The runtime drains the outbox of every terminal, and also right
after each PTY write. `TerminalSubject` admits PTY descriptions: a master
names its slave's terminal with side `pty_master`. On the x86_64 host build the
`SchedulerState` grows by 14848 bytes (8 × 1856: each pair holds a 5968-byte
terminal instead of its 4 KiB slave queue).

## 2. Device nodes — planned (S6)

The nodes live in the kernel `DeviceFileSystem`. The OS mounts it at `/dev`
and calls `attach_terminals(vfs)` before mounting; nothing is mounted at
`/dev/pts`.

| Path | Type | rdev | Inode | Mode | Owner |
|---|---|---|---|---|---|
| `/dev/null`, `zero`, `full` | c | 1,3 / 1,5 / 1,7 | 3/5/7 | 0666 | 0:0 |
| `/dev/tty` | c | 5,0 | 1280 | 0666 | 0:0 |
| `/dev/console` | c | 5,1 | 1281 | 0600 | 0:0 |
| `/dev/ptmx` | c | 5,2 | 1282 | 0666 | 0:0 |
| `/dev/pts` | dir | — | 2 | 0755 | 0:0 |
| `/dev/pts/N` (N < 8) | c | 136,N | 34816+N | 0620 | opener euid:egid |

`/dev/pts` lists every allocated index, locked ones included. Lookup accepts
strict decimal only. An entry disappears when its master's last open closes.

## 3. Opening — planned (S6)

- `/dev/ptmx` allocates a pair and returns the master. The slave is locked and
  dormant, owned by the opener's euid:egid with mode 0620; `grantpt` is a no-op.
- `/dev/pts/N`: absent or stale gives ENOENT; locked or master gone gives EIO;
  access is checked against the entry's mode before EIO. Without `O_NOCTTY`, a
  session leader whose session has no ctty acquires the slave if the slave
  belongs to no session, and the foreground becomes the caller's group.
- `/dev/tty` opens the caller's controlling terminal, or returns ENXIO.
- `/dev/console` opens a console description. It never acquires; reads stay
  lease-gated (EACCES without the lease epoch); writes always succeed.
- Errno order: (1) EINVAL (flags); (2) ENOENT / EACCES (search) / ENOTDIR;
  (3) EMFILE; (4) ENFILE; (5) ENOENT (stale pts); (6) ENOTDIR (`O_DIRECTORY`) or
  EEXIST (`O_CREAT|O_EXCL`); (7) EACCES (node mode); (8) ENXIO / ENOENT / EIO /
  ENOSPC (no free pair) / EOVERFLOW (open count).
- `O_TRUNC` is ignored. `O_NOCTTY`, `O_CLOEXEC`, `O_CREAT`, `O_EXCL` and
  `O_TRUNC` are not kept in `F_GETFL`. The fd is the lowest free one, 0
  included.
- Native `OK_O_NOCTTY` is `0x0100`; Linux `O_NOCTTY` is decoded per profile.
  musl maps ENOSPC from `posix_openpt` to EAGAIN.

## 4. Controlling terminal and foreground group — landed (S4 console, S5 PTY); planned (gates and TIOCNOTTY S8; `/dev/tty` S6)

- **Derivation.** A process's ctty is the terminal whose session equals its
  session ID, unless the process detached itself with TIOCNOTTY as a
  non-leader. The console association also requires the current lease epoch.
  `setsid` detaches; fork and exec preserve the ctty.
- **TIOCSCTTY(arg).** Caller already has this terminal as ctty and is session
  leader: 0. Caller not a session leader, or already has a ctty: EPERM.
  Terminal belongs to another session: EPERM whatever `arg` is (no capability
  model). Fd not readable: EPERM. Otherwise the caller acquires the terminal and
  the foreground becomes its group. On the console only the lease leader may
  acquire. On a PTY master: ENOTTY.
- **TIOCGPGRP / TIOCGSID.** On a slave or the console the terminal must be the
  caller's ctty, else ENOTTY; TIOCGSID with no session gives ENOTTY. On a master
  there is no check and the value is the slave's, except that a raw-pair master
  whose slave has no session answers TIOCGPGRP with ENOTTY (the v1 raw contract
  the OS native PTY fixture asserts; Linux reports 0). PIDs above INT32_MAX give
  EOVERFLOW.
- **TIOCSPGRP** (gated, §9.6). Value < 0: EINVAL. Not the caller's ctty:
  ENOTTY. No such group: ESRCH. Group in another session: EPERM. A group stays
  valid while a zombie member remains.
- **TIOCNOTTY.** From the session leader: SIGHUP then SIGCONT to the foreground
  group, and the association is cleared. From another member: only that
  process detaches. Not the caller's ctty, or a master: ENOTTY.

## 5. ioctl requests — codecs landed (S1); console landed (S4); PTY landed (S5); planned (S8 gate, TIOCNOTTY)

Error order: (1) EBADF; (2) ENOTTY for a non-terminal fd, an unknown request,
or a request invalid on this side; (3) association checks; (4) job-control
gate; (5) EFAULT on copy-in; (6) EINVAL / ESRCH / EPERM; (7) apply; (8) EFAULT
on copy-out. On the console, a caller that is not a lease member gets ENOTTY
for everything except TCGETS and TIOCGWINSZ.

| Request | Native (all ten) | Linux generic¹ | Linux MIPS/MIPS64 | Linux PPC | Argument | Gated |
|---|---|---|---|---|---|---|
| TCGETS | 0x5401 | 0x5401 | 0x540d | 0x402c7413 | out termios | no |
| TCSETS | 0x5402 | 0x5402 | 0x540e | 0x802c7414 | in termios | yes |
| TCSETSW | 0x5403 | 0x5403 | 0x540f | 0x802c7415 | in termios | yes |
| TCSETSF | 0x5404 | 0x5404 | 0x5410 | 0x802c7416 | in termios; flush input first | yes |
| TCSBRK | 0x5409 | 0x5409 | 0x5405 | 0x2000741d | scalar; completes at once | yes |
| TCFLSH | 0x540b | 0x540b | 0x5407 | 0x2000741f | scalar 0..2 over the whole word, else EINVAL | yes |
| TIOCGWINSZ | 0x5413 | 0x5413 | 0x40087468 | 0x40087468 | out 8 bytes | no |
| TIOCSWINSZ | 0x5414 | 0x5414 | 0x80087467 | 0x80087467 | in 8 bytes; SIGWINCH to the foreground on change | no |
| FIONREAD | 0x541b | 0x541b | 0x467f | 0x4004667f | out int | no |
| TIOCNOTTY | 0x5422 | 0x5422 | 0x5471 | 0x5422 | none | no |
| TIOCSCTTY | 0x540e | 0x540e | 0x5480 | 0x540e | scalar | no |
| TIOCGPGRP | 0x540f | 0x540f | 0x40047477 | 0x40047477 | out pid | no |
| TIOCSPGRP | 0x5410 | 0x5410 | 0x80047476 | 0x80047476 | in pid; EIO from the gate becomes ENOTTY | yes |
| TIOCGSID | 0x5429 | 0x5429 | 0x7416 | 0x5429 | out pid | no |
| TIOCGPTN | 0x80045430 | 0x80045430 | 0x40045430 | 0x40045430 | out u32; ptmx master only, else ENOTTY | no |
| TIOCSPTLCK | 0x40045431 | 0x40045431 | 0x80045431 | 0x80045431 | in int; ptmx master only, else ENOTTY | no |

¹ Generic covers i386, x86_64, aarch64, arm32, rv64, rv32 and loongarch64. The
Linux columns are generated from Linux 6.13.4 and musl 1.2.5; the generated
`linux_profile_data.hpp` is authoritative. The rows landed on the console in
S4 and on PTY descriptors in S5, with TIOCGPTN and TIOCSPTLCK
(`OK_TIOCGPTN`, `OK_TIOCSPTLCK`); TIOCNOTTY lands in S8. On a PTY master,
termios, window, foreground and session requests act on the slave's terminal;
TCFLSH and FIONREAD act on the master's own direction (its queue to read, its
input not yet read by the slave). TIOCGPTN and TIOCSPTLCK on a raw-pair master
give ENOTTY: raw pairs never appear in `/dev/pts`.

Every other request gives ENOTTY, including TCXONC, TCGETS2, TIOCPKT, TIOCSTI,
TIOCGPTPEER and TIOCOUTQ. On a hung-up ptmx slave every ioctl gives EIO except
TIOCSPGRP, which gives ENOTTY.

## 6. termios — values landed (S1); console landed (S4); PTY landed (S5)

Native `struct ok_termios` is `{u32 c_iflag, c_oflag, c_cflag, c_lflag; u8
c_line; u8 c_cc[19];}`: 36 bytes, target endian, identical on all ten profiles.
Flag values and cc indices are asm-generic: VINTR 0, VQUIT 1, VERASE 2, VKILL
3, VEOF 4, VTIME 5, VMIN 6, VSWTC 7, VSTART 8, VSTOP 9, VSUSP 10, VEOL 11,
VREPRINT 12, VDISCARD 13, VWERASE 14, VLNEXT 15, VEOL2 16. Speeds come only
from CBAUD. `struct ok_winsize` is 4 × u16.

Linux: the kernel copies exactly the kernel struct size, never the rest of
musl's larger struct.

| Profiles | Size | NCCS | c_line offset | c_cc offset | Speed words |
|---|---|---|---|---|---|
| Generic | 36 | 19 | 16 | 17 | — |
| MIPS/MIPS64 | 40 | 23 | 16 | 17 | — |
| PPC | 44 | 19 | 35 | 16 | at 36 and 40 (as Linux `set_termios` stores them, see below) |

Flag values are mapped through generated tables. Bits and slots without a
generic name (MIPS VDSUSP and slots 18-22, unknown bits) round-trip through
Linux TCGETS/TCSETS only, via the residue. PPC has no TCSETS2, so its speed
words follow `tty_termios_baud_rate`/`tty_termios_input_baud_rate`: the rate
of CBAUD and CIBAUD (CIBAUD B0 is the output rate), the copied word for
BOTHER (kept in the residue, so custom rates round-trip), and 0 for an
encoding without a name.

| Acted on | Flags |
|---|---|
| iflag | ISTRIP, INLCR, IGNCR, ICRNL, IXON (PTY slaves only), IUTF8 |
| oflag | OPOST, ONLCR, OCRNL, ONOCR, ONLRET, TAB3/XTABS |
| lflag | ISIG, ICANON, ECHO, ECHOE, ECHOK, ECHONL, ECHOCTL, ECHOKE, NOFLSH, TOSTOP, IEXTEN |
| cc | VINTR, VQUIT, VSUSP, VEOF, VEOL, VEOL2, VERASE, VKILL, VWERASE, VLNEXT, VREPRINT, VSTART, VSTOP, VMIN, VTIME (PTY only; the console treats VTIME as 0) |

Stored but not acted on: PARMRK, INPCK, IGNPAR, BRKINT, IGNBRK, IUCLC, IXANY,
IXOFF, IMAXBEL (Linux n_tty never rings on overflow either), OLCUC, fill and
delay bits, XCASE, ECHOPRT, FLUSHO, PENDIN, EXTPROC, VDISCARD, CBAUD
reprogramming.

| Terminal | iflag | oflag | cflag | lflag | Window |
|---|---|---|---|---|---|
| Console (`console_compat`; reset on lease claim, revoke and retire) | ICRNL | 0 | B115200\|CS8\|CREAD\|CLOCAL | 0 | 24×80 |
| `/dev/ptmx` pairs (pts `init_termios`: `tty_std_termios` without HUPCL) | ICRNL\|IXON | OPOST\|ONLCR | B38400\|CS8\|CREAD | ISIG\|ICANON\|ECHO\|ECHOE\|ECHOK\|ECHOCTL\|ECHOKE\|IEXTEN | 0×0 |
| `OK_SYS_PTY_PAIR` raw pairs (`cfmakeraw(tty_std)`) | 0 | ONLCR (OPOST off) | B38400\|CS8\|CREAD\|HUPCL | ECHOE\|ECHOK\|ECHOCTL\|ECHOKE | 0×0 |

All three use the standard cc set: ^C ^\ DEL ^U ^D, VTIME 0, VMIN 1, ^Q ^S ^Z,
VEOL 0, ^R ^O ^W ^V, VEOL2 0. The console default reproduces today's
behaviour (CR→LF, no echo, byte at a time) and stays the kernel default
permanently: a getty sets cooked mode itself, and login clears ECHO for the
password.

## 7. N_TTY behaviour — model landed (S1); console landed (S4); PTY landed (S5); planned (S7 PTY VTIME)

Until S7 a PTY read that would arm the VTIME timer waits for input instead.

- The read buffer is 4096 bytes, and canonical input follows Linux
  `n_tty_receive_buf_common`. Once 4095 bytes are buffered, every byte waits
  while a complete line is queued. Without one, input goes on, and a byte
  arriving at a full buffer first takes back the newest stored byte. So an
  ordinary byte overwrites the last slot (and is echoed under ECHO), erase,
  kill, newline and EOF keep working, and the longest line is 4095 bytes plus
  the terminator.
- In non-canonical mode a full buffer (4096 bytes, §14) applies backpressure
  to a PTY master writer, as does canonical input that has to wait. On the
  console such a byte is dropped and counted as an overrun.
- A canonical read returns at most one line. VEOF at the start of a line
  returns 0.
- VMIN/VTIME follow POSIX. A VMIN read is admitted atomically when enough bytes
  are present; competing readers do not consume bytes progressively.
- Echo goes through OPOST; its ring holds 512 bytes and drops (and counts) the
  oldest bytes on overflow. A terminal write first flushes pending echo. As
  in Linux, the output column moves only for echo that is written (taken by
  the terminal): echo discarded by an ISIG flush or dropped on overflow
  leaves it where it was.
- ISIG: VINTR → SIGINT, VQUIT → SIGQUIT, VSUSP → SIGTSTP, to the foreground
  group. Unless NOFLSH is set, input and the peer's queued output are flushed.
  With no foreground group nothing is sent.
- IXON VSTOP/VSTART stop and start PTY slave output; the console ignores
  output stop.
- `TCSETS*` that changes ICANON discards no input, as Linux
  `n_tty_set_termios`: turning ICANON off makes every buffered byte readable,
  turning it on with input buffered ends that input as one line. Only
  `TCSETSF`, TCIFLUSH and an ISIG character without NOFLSH discard input.
- Console output is synchronous: `write` and `writev` return the full count
  after OPOST, up to 64 KiB per call (a longer write returns 65536 and the
  caller writes the rest). Console reads copy out before they consume, so a
  faulting buffer loses no input.
- `OK_SYS_CONSOLE_FLUSH` also flushes the console line discipline's input.

## 8. PTY pair states — data and open counts landed (S5); planned (S6 `/dev/pts`; S8 signals)

| Event | ptmx pair (Linux semantics) | `OK_SYS_PTY_PAIR` raw pair (v1, unchanged) |
|---|---|---|
| Slave never opened (dormant) | master read blocks; master write EIO | n/a (both ends open at creation) |
| Slave's last close | master drains, then EIO; POLLHUP. Slave input and session are kept; the slave may be reopened while unlocked and the master lives. | master drains, then EIO; POLLHUP |
| Master's last close | `/dev/pts/N` disappears. The slave discards input and reads 0; writes EIO; ioctls EIO (TIOCSPGRP ENOTTY); poll IN\|OUT\|ERR\|HUP. SIGHUP then SIGCONT go to the session leader, and the session's ctty is cleared. | slave drains, then EOF; poll HUP\|ERR, plus IN while data remains, never OUT. Session hangup signals as for ptmx. |
| Both sides at 0 opens | slot freed | slot freed |

A slave can have several open descriptions. fork, dup and SCM_RIGHTS share
descriptions and do not change open counts. TIOCSPTLCK relocking only refuses
new slave opens.

Landed (S5): every endpoint counts its open descriptions (u16), and only a
prepared raw pair or the bind calls create PTY descriptions
(`allocate_description` refuses `pty_endpoint` with EINVAL). The final
release of a description retires one open; the slot is freed at 0 + 0.
`SchedulerState::bind_pty_multiplexer(description)` creates a ptmx-style pair
(locked, dormant slave, `linux_pty_termios()`) and returns the master endpoint;
`bind_pty_slave(description, slave)` admits one slave open: EBADF for an
invalid handle, ENOENT for a stale generation, EIO when the master is gone, a
raw slave was closed or the slave is locked, EOVERFLOW at 65535 opens. A
description to bind must be fresh: provider-free with exactly one reference.
`pty_index` and `set_pty_lock` serve TIOCGPTN/TIOCSPTLCK, and
`terminal_audit()` checks that every endpoint's open count equals its bound
descriptions. The hung-up data rules are landed; the hangup signals and the
cleared session wait for S8 (a hung-up slave keeps its session until then).

## 9. Job control

### 9.1 Numbers — landed (S3)

Native numbers are uniform on all ten profiles. `OK_SIGKILL` and `OK_SIGTERM`
stay in `ok/uapi/signal.h`; the others are defined in `ok/uapi/job_control.h`.
Linux numbers follow the profile. The kernel translates between them; across
personalities on MIPS, a signal with no counterpart gives EINVAL.

| Signal | Native = Linux generic | Linux MIPS/MIPS64 |
|---|---|---|
| SIGHUP | 1 | 1 |
| SIGINT | 2 | 2 |
| SIGQUIT | 3 | 3 |
| SIGKILL | 9 | 9 |
| SIGTERM | 15 | 15 |
| SIGCHLD | 17 | 18 |
| SIGCONT | 18 | 25 |
| SIGSTOP | 19 | 23 |
| SIGTSTP | 20 | 24 |
| SIGTTIN | 21 | 26 |
| SIGTTOU | 22 | 27 |
| SIGWINCH | 28 | 20 |

### 9.2 Default actions — landed (S2 delivery, S3 generation)

CHLD, WINCH and URG are ignored. CONT continues. STOP, TSTP, TTIN and TTOU
stop. Every other signal terminates. No core-dump bit is set.

Landed in S2: a queued default STOP/TSTP/TTIN/TTOU is consumed by Linux
delivery without a frame and stops the process. A pending default stop or a
default-ignored signal never interrupts a wait (pipe, child, futex, poll,
sleep, UDP, sigsuspend); a stop that only threads parked in waits can take is
applied at the next scheduler step, and those waits stay armed. That step takes
the stop only when it is the signal the thread would dequeue next (directed
queue, then shared, lowest number first, ignored signals passed over), so a
lower-numbered fatal or caught signal is delivered first. A ppoll or pselect
interrupted by a caught signal keeps its temporary mask across a stop, and the
handler runs under it after the continue.

### 9.3 Stop and continue — landed (S2 overlay, S3 generation)

- A stop is a per-process overlay: the process is never entered while stopped.
  Kernel waits it started still complete, and their results are published when
  it resumes.
- Generating SIGCONT continues the process even if SIGCONT is ignored or
  blocked, and discards pending STOP/TSTP/TTIN/TTOU. Generating any stop signal
  discards a pending SIGCONT.
- Only SIGKILL acts on a stopped process. Any other signal sent to a stopped
  Linux process, terminating defaults included, stays pending and acts after
  the continue (Linux `wants_signal`); a stopped native process applies its
  recorded terminating default after the continue. A process already
  condemned by a fatal signal no longer stops.
- A stop applies when the signal is generated, if it is deliverable then: some
  thread does not block it and no signal that thread would dequeue first is
  pending. Otherwise it is queued and applies when delivery or the settle pass
  reaches it.
- A terminating default sent to a running Linux process none of whose
  unblocking threads can take a signal now terminates it (Linux
  `complete_signal` fatal shortcut) before any of its threads runs again,
  also when a timer or an owed kernel signal decided it.
- TSTP/TTIN/TTOU in an orphaned group are discarded when they would apply: at
  generation when deliverable, otherwise when dequeued (a blocked one stays
  pending for `rt_sigpending` and `sigtimedwait`). SIGSTOP never is.
- Init (pid 1) follows Linux `SIGNAL_UNKILLABLE` in `sig_ignored` order: a
  SIG_DFL terminating or stopping signal, KILL and STOP included, is ignored
  at generation unless the named thread blocks it; a blocked one is queued,
  never completes a wait, and is dropped when dequeued. The CONT/stop
  discards still run first. This covers kill, tkill/tgkill, POSIX timers
  (process- and thread-directed), SIGPIPE and kernel senders. Native kill of
  pid 1 stays EPERM.
- SIGCONT may be sent to any process in the sender's session, whatever the
  credentials.

### 9.4 SIGCHLD and waits — landed (S3)

- The parent receives SIGCHLD with CLD_STOPPED (5) or CLD_CONTINUED (6),
  status = the signal in the parent's numbering, unless the parent is native,
  its SIGCHLD action is SIG_IGN, SA_NOCLDSTOP (1) is set, or SIGCHLD is neither
  caught nor blocked. CLD_CONTINUED is queued when SIGCONT is generated. Like
  exit notifications, a native child sends no SIGCHLD; its waits still report.
- wait4 options are WNOHANG 1, WUNTRACED 2 and WCONTINUED 8; anything else is
  EINVAL. Selectors: pid, −1, 0 (the caller's group at call time) and −pgid;
  INT_MIN gives ESRCH. `rusage` stays EINVAL.
- Status: stopped `(sig<<8)|0x7f`; continued `0xffff`; exit and termination
  unchanged, with the termination signal translated to the parent's numbering.
- RV32 `waitid`: WEXITED 4, WSTOPPED 2, WCONTINUED 8, WNOWAIT 0x01000000;
  P_PID, P_PGID (0 = own group) and P_ALL. Other profiles keep ENOSYS. A group
  selector compares each child's group when the wait is checked. Without
  WEXITED a zombie child is no candidate, so a caller with only zombies gets
  ECHILD instead of 0 or a wait.
- A blocked group wait (selector 0 or −pgid, P_PGID) is interrupted by a
  caught signal like any child wait: EINTR, or a restart under SA_RESTART.
- Native wait4 takes `OK_WNOHANG`, `OK_WUNTRACED` and `OK_WCONTINUED` from
  `ok/uapi/job_control.h`, with the same values.

### 9.5 Native dispositions — landed (S3)

Native `rt_sigaction(sig, act, oact, 8)` checks, in order:

| Check | Result |
|---|---|
| sigsetsize ≠ 8 | EINVAL |
| `act` copy fault | EFAULT |
| sig ∉ 1..64 | EINVAL |
| `act` for KILL or STOP | EINVAL |
| `act.handler` ∉ {`OK_SIG_DFL` 0, `OK_SIG_IGN` 1} | EINVAL |
| non-zero `flags` or `mask` | EINVAL |
| SIG_IGN for SIGCHLD | EINVAL |
| commit, then `oact` write fault | EFAULT (after commit) |

`struct ok_sigaction {u64 handler, flags, mask;}` is 24 bytes and lives in
`ok/uapi/job_control.h` with `OK_NATIVE_SIGNAL_ACTION_VERSION`. SIG_IGN is
inherited across fork and kept across exec, except that a SIGCHLD SIG_IGN
inherited from a Linux image reverts to SIG_DFL on exec into the native
personality (native has no autoreap). `rt_sigprocmask` stays a no-op and
native processes have no masks, so a native program that "blocks" SIGTTOU is
stopped; it must ignore the signal instead. Native recipients accept every
signal: terminating defaults terminate, stop defaults stop. `tgkill` of a
native thread returns EOPNOTSUPP.

### 9.6 Background access — planned (S8)

Linux `__tty_check_change`. Applies only when the terminal is the caller's
ctty, has a foreground group, and the caller is not in it.

| Operation | SIGTTIN/SIGTTOU ignored, or blocked (Linux) | Group orphaned | Otherwise |
|---|---|---|---|
| read | EIO | EIO | SIGTTIN to the caller's group; the call restarts when the caller is continued, or EINTR / SA_RESTART under a Linux handler |
| write with TOSTOP | allowed | EIO | SIGTTOU to the caller's group; same restart rule |
| TCSETS\*, TCFLSH, TCSBRK, TIOCSPGRP | allowed | EIO (ENOTTY for TIOCSPGRP) | SIGTTOU to the caller's group; same restart rule |

- If the caller would neither stop nor have the call interrupted (pid 1), the
  result is EIO.
- The check runs at call entry, on restart, and after the caller is continued
  from a stop taken while blocked on the terminal.
- Callers whose ctty is a different terminal are not job-controlled; this
  includes console lease members in other sessions.

### 9.7 Hangup and related signals

| Event | Signals | Status |
|---|---|---|
| PTY master's last close | SIGHUP then SIGCONT to the slave session's leader; ctty cleared for the session | planned (S8) |
| Session leader exit, ctty is a PTY | SIGHUP to the foreground group; association cleared | planned (S8) |
| Session leader exit, ctty is the console | SIGHUP to the foreground group; association cleared; no console vhangup | planned (S8) |
| TIOCNOTTY by the leader | SIGHUP then SIGCONT to the foreground group | planned (S8) |
| Console lease revoke or retire | SIGHUP then SIGCONT to the console session leader, then the console terminal is reset | planned (S8) |
| A process exit orphans a group with stopped members | SIGHUP then SIGCONT to that group, after any terminal hangup signals | landed (S3) |
| Window size change | SIGWINCH to the foreground group | landed (S4 console, S5 PTY) |

Terminal and orphan signals are kernel-originated (SI_KERNEL, code 128). They
bypass permission checks and are translated per recipient. The kernel sender
API (`send_kernel_process_signal`, `send_kernel_group_signal`, native numbers)
is landed (S3). On the console, VINTR, VQUIT and VSUSP under ISIG send SIGINT,
SIGQUIT and SIGTSTP to the foreground group, and a TIOCSWINSZ that changes the
size sends SIGWINCH (S4); PTY master input and PTY TIOCSWINSZ do the same
(S5); the hangup events arrive in S8.

### 9.8 Kill-path errors — landed (S3)

- kill, tkill and tgkill accept every signal number. Init (pid 1) is
  protected as in §9.3 for every sender, timers and SIGPIPE included.
- A caught signal sent by a user to a running Linux process whose unblocked
  threads are all in waits that cannot be interrupted returns EOPNOTSUPP and
  has no effect. SIGCONT and the stop signals are the exception: their
  discards and the continue happen at generation, so they are queued and the
  call returns 0. The kernel's own signals in that case are retried (owed)
  until such a wait completes and are not shown by `rt_sigpending`; owed
  signals survive an exec in the same personality and are dropped by an exec
  into the other one.
- `kill(-1)` returns EOPNOTSUPP.

## 10. Readiness — console landed (S4); native PTY landed (S5); planned (S7 Linux poll; S8 console POLLERR)

Applies to poll and select in both personalities.

| Endpoint | Readable | Writable | Error and hangup |
|---|---|---|---|
| Slave | a line (canonical), or ≥ 1 byte / VMIN (raw); or hung up | room and echo flushed, and not hung up (ptmx hung up: IN\|OUT\|ERR\|HUP) | per §8 |
| Master | queued output; or slave closed | slave open and ldisc room | HUP when the slave is closed |
| Console | ldisc readable, for lease members | always | no foreground-based POLLERR |

## 11. errno summary — planned (S6 constants)

Beyond §3 and §5:

| errno | Value | Meaning |
|---|---|---|
| EINTR | 4 | Linux handler interruption |
| EIO | 5 | background read when ignored/blocked/orphaned; hung-up slave write or ioctl; master write with no open slave; locked slave open |
| ENXIO | 6 | `/dev/tty` without a ctty |
| ENOTTY | 25 | non-terminal fd; unknown request; TIOCSPGRP gate EIO |
| ENOSPC | 28 | no free PTY slot |
| EOPNOTSUPP | 95 | see §9.8 |

## 12. Capacities — N_TTY and termios landed (S1); PTY landed (S5)

| Item | Value |
|---|---|
| PTY pairs | 8, shared between `/dev/ptmx` and `OK_SYS_PTY_PAIR` (ptmx gets 8 minus live raw pairs) |
| Terminals | 9 |
| N_TTY read buffer | 4096 bytes |
| Echo ring | 512 bytes |
| Master queue | 4096 bytes |
| Terminal signal outbox | 8 per terminal |
| Processes | 64 |
| Group-signal snapshot | 64 |
| Owed kernel-signal records | 64 (signals < 32) |
| Termios bytes | native 36; Linux 36/40/44 |
| winsize | 8 bytes |
| PID wire | 4 bytes |

## 13. SDK markers and ABI — planned (S9a markers and UAPI 0.17; S9b ABI 79)

| Header | String | Version | Changes |
|---|---|---|---|
| `native_pty_profile.hpp` | `bootcpu-native-pair-devpts-counted-endpoints-ntty-hangup-v2` | 2 | `linux_creation`, `slave_reopen`, `termios`, `controlling_terminal`, `terminal_signals`, `job_control`, `devpts` = 1; `raw` stays 1; added `open_counts`, `dormant_slave_read_block`, `master_slave_closed_eio`, `ptmx_hangup_discard`, `raw_hangup_drain`, `canonical_capacity=4096`, `echo_capacity=512`; `scm_rights=0`, `smp=0` kept |
| `native_console_control_profile.hpp` | `bootcpu-lease-epoch-explicit-console-pty-session-foreground-sigttin-sigttou-pid32-ioctl-v2` | 2 | `background_eio=0`; `pty`, `termios`, `stop_continue`, `job_control` = 1; `auto_open_acquire=1` (pts only); `force_steal=0` kept; added `background_sigttin=1`, `background_sigttou=1`, `console_auto_open_acquire=0`, `dev_tty=1`, `noctty_open=1` |
| `native_sessions_profile.hpp` | `bootcpu-serialized-process-group-session-console-pty-jobctl-lifecycle-v4` | 4 | `pty`, `job_control` = 1; added `orphaned_group_hup=1`, `ctty_derived=1` |
| `native_group_signal_profile.hpp` | `bootcpu-signed32-current-negative-pgid-probe-all-signals-stop-continue-orphan-hupcont-same-session-cont-partial-v3` | 3 | `stop_continue`, `job_control` = 1; added `default_actions`, `same_session_continue`, `orphan_hangup` = 1 |
| `sched/console_input_profile.hpp` | `pid1-issued-epoch-fork-exec-leader-revoke-ntty-compat-default-v2` | — | — |
| `native_tty_profile.hpp` (new) | `bootcpu-ntty-template-console-pts8-devfs-ptmx-tty-console-v1` | 1 | §2/§12 numbers; acted/not-acted flags (§6); `console_vtime=0`; `console_vhangup=0`; `console_ixon=0`; `native_termios_bytes=36`; `linux_codec_revision=3`; `pts_mode=0620`, `pts_owner_opener=1`, `grantpt_noop=1`, `locked_listed=1`, `separate_devpts_mount=0`, `stat_rdev_linux=1`, `stat_rdev_native=0`, `tiocgptpeer=0`, `packet_mode=0`, `smp=0` |
| `native_job_control_profile.hpp` (new) | `bootcpu-overlay-stop-cont-wuntraced-wcontinued-pgrp-wait-nocldstop-owed-kernel-v1` | 1 | `wait_untraced`, `wait_continued`, `group_wait`, `waitid_pgid` (RV32), `nowait` (RV32), `nocldstop`, `native_dfl_ign`, `init_protection` = 1; `native_handlers`, `native_masks`, `broadcast`, `ptrace`, `smp` = 0 |

Each marker also carries `module_abi=79`. The SDK moves to UAPI 0.17.0 in S9a
and module ABI 79 in S9b, each bumped once, at the end. Until then every
profile string and version above keeps its current value.

## 14. Deviations from Linux

| # | Deviation | Status |
|---|---|---|
| 1 | A stop applies at generation when deliverable. While stopped, `sigtimedwait`, `epoll_wait` and similar calls keep waiting instead of returning EINTR on continue. | landed (S2, S3) |
| 2 | Kernel signals to threads in waits that cannot be interrupted are retried (owed) and not visible in `sigpending`; an exec into the other personality drops them. A user's caught signal other than SIGCONT and the stops gets EOPNOTSUPP in that case. | landed (S3) |
| 3 | Native personality: SIG_DFL/SIG_IGN only, no masks, SIGCHLD SIG_IGN refused (an inherited one reverts at exec), tgkill to native refused. A native child sends no SIGCHLD. A native recipient records the first terminating default at generation instead of queueing it. | landed (S3) |
| 4 | No ptrace stops, no `kill(-1)`, no core bit. `waitid` is RV32-only. | landed (S3) |
| 5 | No devpts filesystem: no mount options, no `newinstance`, no `/dev/pts/ptmx`. `st_dev(/dev/pts/N) == st_dev(/dev)`. The gid policy is the opener's egid. | planned (S6) |
| 6 | 8 PTYs, shared with raw pairs. | landed (S5); `/dev/ptmx` planned (S6) |
| 7 | No console vhangup. VTIME on the console is treated as 0. IXON output stop works on PTY slaves only. | VTIME landed (S4); IXON landed (S5); planned (S8) |
| 8 | VMIN reads are admitted atomically. | landed (S1) |
| 9 | Echo overflow and console overrun drop bytes and are counted. Echo kept after a drop keeps the OPOST expansion (XTABS, tab-erase width) computed when it was generated. | landed (S1, S4) |
| 10 | A PTY master cannot be a controlling terminal. | landed (S5) |
| 11 | Native `stat` has no rdev, and there is no `ttyname` (no `/proc`). | planned (S6) |
| 12 | After a hangup and a later re-acquire by the leader, members forked earlier see the new ctty. | planned (S8) |
| 13 | pid 1 gets EIO instead of a restart loop on background terminal access. | planned (S8) |
| 14 | Non-canonical input fills all 4096 bytes of the read buffer (Linux stops at 4095), so raw pairs keep their 4096-byte queue. | landed (S1) |
| 15 | A raw-pair (`OK_SYS_PTY_PAIR`) master whose slave has no session answers TIOCGPGRP with ENOTTY instead of 0, keeping the v1 raw contract. | landed (S5) |
| 15 | Init never takes the `complete_signal` fatal shortcut. Linux 6.13 does when the named thread blocks a SIG_DFL fatal signal that another init thread does not; here it stays queued and is dropped when dequeued. | landed (S3) |
| 16 | One console `write` or `writev` moves at most 64 KiB; a longer one returns a short count. | landed (S4) |

## Tests

Hosted runners (model/hosted evidence only):

```sh
python3 -B scripts/test_native_pty.py --output <dir>
python3 -B scripts/test_tty_sdk_contract.py --output <dir> --os-root <ObfuscationOS checkout>
python3 -B scripts/test_n_tty_model.py --output <dir>
python3 -B scripts/test_terminal_codec.py --output <dir>
```

`test_native_pty.py` builds every target of `tests/host/native_pty/xmake.lua`
with `--runtime_tests=y` from a source snapshot and requires each test
binary's exact completion line (`PRIVATE_PTY_ACTUAL_OFD`,
`PRIVATE_RAW_PTY_OWNER … model_steps=100000`, `NATIVE_PTY_PAIR_BOUNDARY_PASS`,
`PTY_RUNTIME_PASS actual_runtime=1 hosted=1 guest=0`, the boot-check model and
the UNIX rights regressions). `test_tty_sdk_contract.py` runs the PTY and
console-input SDK contract suites against the given consumer and prints
`TTY_SDK_CONTRACT_PASS suites=2 guest=0`; the hosted suite lists it under
`inputs`.

S1 (value layer; nothing under `src/` uses it yet):

- `include/ok/tty/termios.hpp`: `Termios`, `WinSize`, `ForeignResidue`,
  `linux_std_termios()`, `console_compat_termios()`, `raw_pair_termios()` and
  the 36-byte `NativeTermiosWire<BigEndian>`.
- `include/ok/tty/n_tty.hpp`: the constant-initializable `NTty<4096, 512>`
  template. It reports signals, flushes and readiness to its owner, returns
  VMIN/VTIME waits and timer lengths instead of keeping a clock, and leaves
  echo and output bytes for the owner to move.
- `include/ok/abi/linux_termios.hpp`: `TermiosCodec<Profile>` and
  `TtyIoctlCodec<Profile>` over the generated tables (codec revision 3).
- `test_n_tty_model.py` requires `N_TTY_MODEL_PASS canonical=4096 echo=512
  model_steps=100000 utf8_erase=1 isig=1 vmin_vtime=1 opost=1
  raw_equivalence=1 constexpr=1 guest=0`: directed Linux n_tty cases, a seeded
  differential run against a string reference, and `raw_pair_termios()` as a
  4096-byte FIFO.
- `test_terminal_codec.py` requires `TERMINAL_CODEC_PASS linux_profiles=10
  termios_layouts=3 roundtrip=1 residue=1 native_wire_endians=2 hosted=1
  guest=0`. It also compiles `ok/uapi/termios.h` as freestanding C11 and as
  freestanding C++23 (checked against `ok/tty/termios.hpp`), compares its
  macros with the generated asm-generic values, and fails when a header the
  test compiles is missing from its source pins.
- Debug test points `tty/n-tty-line-discipline-model` and
  `tty/termios-codec-model` (configured profile) are models booted in QEMU.

S2 adds `python3 -B scripts/test_job_control_model.py --output <dir>`, which
prints `JOB_CONTROL_MODEL_PASS profiles=10 numberings=2 orphan_cases=<n>
guest=0`: Linux generic and MIPS numbering by name, default actions, codes 5/6
and the SI_KERNEL set on all ten profiles, the actual scheduler stop overlay,
and the orphaned-group rule against a brute-force oracle over every parent,
group and session assignment of five processes. The debug test point
`job-control / scheduler-overlay-orphan-model` is the same kind of model.

S3 adds `python3 -B scripts/test_native_job_control.py --output <dir>`, which
prints `NATIVE_JOB_CONTROL_RUNTIME_PASS native_calls=<n> linux_calls=<n>
profiles=10 hosted=1 guest=0`. It drives the actual runtime (one translation
unit, x86_64 hosted backends): Linux kill, tkill and group stop/continue and
native kill for both personalities, the CONT/stop discards, KILL and held fatal
defaults while stopped, a stopped thread never entered by `step()`, wait4 on
selectors 0 and -pgid, SA_NOCLDSTOP and blocked SIGCHLD, native
`rt_sigaction` error order and fork inheritance, init protection in
`sig_ignored` order (blocked signals queued then dropped, waits not completed,
thread-directed timers, SIGPIPE, a second unblocking thread), same-session
SIGCONT for kill, native kill and tkill, the orphaned-group TSTP drop at
generation and at dequeue, exit-time SIGHUP then SIGCONT for adopted
children's groups and the exiting process's own group, caught SIGCONT and
stops queued rather than refused, owed kernel signals (several drained in one
pass, a partial retry keeping the rest, CONT/SIG_IGN/pid-reuse drops, kept by
a same-personality exec, dropped by a personality change, which also resets
SIGCHLD SIG_IGN), a timer retry that neither repeats its CONT nor survives the
opposite stop, a fatal decision applied in the step that made it, and
shutdown's terminate path with a stopped process. The ten profiles are covered
for numbering and translation only. `test_native_child_wait_signal.py`
(quarantined in the hosted suite, whose all-profile adapter section still
fails at origin capture) also covers caught-signal interruption of group
waits and RV32 `waitid` without WEXITED before that section.

S4 adds two runners on the actual runtime (x86_64 hosted backends):

- `python3 -B scripts/test_terminal_control.py --output <dir>` prints
  `TERMINAL_CONTROL_PASS native_requests=15 linux_profiles=10 console=1
  error_order=1 hosted=1 guest=0`: the shared dispatch's error order (EBADF,
  ENOTTY, association, copy-in EFAULT, control errors, EOVERFLOW, copy-out
  EFAULT) and exact copy sizes for the native wire on both endiannesses and all
  ten Linux encodings, the native request table, and a Linux TCSETS on the
  actual console read back by native TCGETS and, byte for byte, by Linux TCGETS
  on every profile.
- `python3 -B scripts/test_native_tty.py --output <dir>` prints
  `NATIVE_TTY_RUNTIME_PASS console=1 compat_default=1 icanon=1 echo=1 isig=1
  winch=1 flush=1 writev_8k=1 efault_keeps_input=1 vmin_wake=1 hosted=1 guest=0`,
  observing
  console output through a mock `ok_platform_console_write_char`: the compat
  default (no echo, no ONLCR, CR read as LF), canonical input with erase echo
  and VEOF, ISIG to the foreground group (NOFLSH, ISIG off, ^Z stop), SIGWINCH,
  `OK_SYS_CONSOLE_FLUSH`/TCFLSH/TCSETSF, repeated 8 KiB writevs, EFAULT on
  read and readv keeping the input, and non-canonical reads woken by every
  buffered byte and by a TCSETS that lowers VMIN.

`test_console_input.py` now requires `ntty_compat_default=1` instead of
`noecho=1` (the 4096-byte ldisc replaced the lease's 256-byte ring), and
`test_native_console_control.py` covers the termios, window, flush, FIONREAD
and TCSBRK rows and TIOCSCTTY without EOPNOTSUPP.

S5 extends existing runners (hosted model and runtime evidence only):

- `test_native_pty.py` requires `PRIVATE_RAW_PTY_OWNER cases=<n> checks=<n>
  model_steps=100000 open_counts=1 raw_termios=1 status=passed`. The deque
  oracle now drives raw and ptmx-style pairs (the latter switched to raw
  termios) through creation, counted slave opens, locks, retirement and
  hangup, and directed cases cover raw termios, canonical echo, ISIG and its
  outbox backpressure, IXON, echo waiting for master room, TCFLSH, slave
  reopen and the hangup discard. `native_pty_ofd_tests.cpp` checks
  `terminal_audit` after every operation, refuses PTY endpoints in
  `allocate_description` (EINVAL), sees both raw ends as terminal
  descriptions, counts several slave descriptions across dup, fork and
  SCM_RIGHTS, and frees a slot only at zero opens.
- `test_native_tty.py` adds `pty=1 canonical_echo=1 ptmx_hangup_discard=1
  raw_drain=1 ptn_lock=1` before `hosted=1 guest=0`.
- `test_terminal_control.py` maps `OK_TIOCGPTN` and `OK_TIOCSPTLCK` natively
  (`native_requests=15`).
- The `native-pty/scheduler-OFD-ownership-model` debug point also runs a
  ptmx-style pair (counted opens, canonical echo, hangup discard).

Each slice adds its runners here.
