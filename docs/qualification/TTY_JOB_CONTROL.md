# Terminals and job control

WT2.2 `feat/tty-jobctl`, paired with the ObfuscationOS worktree
`feat/getty-login`. The contract is [tty](../modules/tty.md); this note is the
per-slice log: what each slice landed, which evidence backs it, and the
deviations it recorded. Slices S0-S9a are landed; S9b (module ABI 80) is made
at merge.

## Evidence classes

- **Hosted model/runtime.** `scripts/test_*.py` runners and the SDK contract
  suites, run by `scripts/hosted_suite.py`. Runtime runners compile the actual
  runtime units as one translation unit with x86_64 hosted backends and enter
  syscalls through `step()`; the ten profiles are covered for numbering,
  encodings and translation. These are model evidence only.
- **QEMU debug test points.** `xmake test` boots the debug kernel on a profile
  and runs its registered test points (86 at S8 and S9a). They are models booted
  in QEMU; standalone `xmake test` never starts native userland.
- **Guest execution.** Only ObfuscationOS images built against the exported
  SDK execute native or Linux userland: `qemu-distro --fs=simplefs
  --require-native`, the interactive prompt driver and the guest probes
  (`TTY_DEVPTS_PROBE`, `TTY_LINUX_PROBE`). During the slices these ran on
  `git archive` copies of OS main with the paired changes applied; the OS
  checkout and its paused worktree were not edited.

No slice claims SMP, physical hardware or process isolation.

## Slice log

| Slice | Landed | Hosted (model/runtime) | QEMU debug points | Guest |
|---|---|---|---|---|
| S0 | PTY suites as a hosted runner, the terminal SDK contract runner, the published contract | `test_native_pty.py`, `test_tty_sdk_contract.py` (suites=2) | — | — |
| S1 | `Termios`, `NTty<4096, 512>`, generated Linux termios and ioctl codecs (revision 3), `ok/uapi/termios.h` | `test_n_tty_model.py` (`N_TTY_MODEL_PASS`), `test_terminal_codec.py` (`TERMINAL_CODEC_PASS linux_profiles=10`) | `tty/n-tty-line-discipline-model`, `tty/termios-codec-model` | — |
| S2 | stop overlay, default-stop delivery, CLD codes | `test_job_control_model.py` (`JOB_CONTROL_MODEL_PASS profiles=10`) | `job-control/scheduler-overlay-orphan-model` | — |
| S3 | one signal generation core, native SIG_DFL/SIG_IGN, WUNTRACED/WCONTINUED and group waits, SIGCHLD stop reports, orphaned-group HUP+CONT, owed kernel signals, `ok/uapi/job_control.h` | `test_native_job_control.py` (`NATIVE_JOB_CONTROL_RUNTIME_PASS profiles=10`) | ten-profile `xmake test` | unmodified OS `qemu-distro`, x86_64 |
| S4 | console terminal object with N_TTY under `console_compat`, one terminal dispatch, ISIG and SIGWINCH | `test_terminal_control.py`, `test_native_tty.py`, `test_console_input.py` (`ntty_compat_default=1`), `test_native_console_control.py` | fast lanes | OS `qemu-distro` and interactive `--posix-check`, x86_64, mips, ppc |
| S5 | PTY slaves as terminals, counted endpoint opens, PTY ioctls, ptmx hangup data rules | `test_native_pty.py` (`open_counts=1 raw_termios=1`), `test_native_tty.py` (`pty=1`) | `native-pty/scheduler-OFD-ownership-model` with a ptmx pair | OS `qemu-distro` and the native PTY fixture, x86_64 and ppc |
| S6 | `/dev/tty`, `/dev/console`, `/dev/ptmx`, `/dev/pts/N` in devfs, the terminal open path, `st_rdev`, `OK_O_NOCTTY` | `test_devfs_terminals.py`, `test_tty_open.py` (`profiles=10`), `test_linux_openat.py` | `devfs/terminal-namespace-model`, `tty-open/ptmx-pts-ctty-model` | `TTY_DEVPTS_PROBE stage=PASS`, x86_64 (paired OS copy with `attach_terminals`) |
| S7 | Linux PTY I/O and poll, EINTR/SA_RESTART on terminal waits, PTY VTIME | `test_native_terminal_signal.py` (`profiles=10 eintr=1 restart=1 vtime=1`), `test_native_pty.py` | fast lanes | `TTY_LINUX_PROBE stage=PASS checks=43 interrupts=2`, x86_64 and aarch64 (static musl) |
| S8 | SIGTTIN/SIGTTOU gates and TOSTOP, every hangup of section 9.7, TIOCNOTTY | `test_native_tty.py` (`sigttin=1 sigttou=1 tostop=1 hangup=1 tiocnotty=1`), `test_native_terminal_signal.py` (`job_control_park=1`), `test_process_sessions.py` (`tty=1`), `test_native_console_control.py` | x86_64, arm32, ppc, mips | paired OS `qemu-distro` and interactive `--posix-check`, x86_64, mips, ppc; unmodified OS main still passes on x86_64 |
| S9a | seven SDK markers (two new), UAPI 0.17.0, exporter validators and manifest keys, final docs | `test_tty_sdk_contract.py` (suites=3), `test_native_signal_export.py`, every `*_sdk_contract_tests.py`, `check_runtime_split.py` (671 lines), full hosted suite | x86_64, arm32, ppc, mips (86 points) | paired OS consumer: `kernel-sdk-check`, `qemu-distro`, x86_64; the OS tools unittests |
| S9b | module ABI 80 | — | — | — (at merge) |

Each slice's runners, their exact completion lines and the cases they cover
are listed in the Tests section of [tty](../modules/tty.md).

## S9a: SDK markers and UAPI 0.17

- Markers (section 13 of the contract): `native_pty_profile` v2,
  `native_console_control_profile` v2, `native_sessions_profile` v4,
  `native_group_signal_profile` v3, `native_console_input_profile`
  `...-ntty-compat-default-v2`, and the new `native_tty_profile` v1 and
  `native_job_control_profile` v1. All keep `module_abi = 79`.
- UAPI 0.17.0: `OK_NATIVE_SESSIONS_VERSION 4`,
  `OK_NATIVE_CONSOLE_CONTROL_VERSION 2`, `OK_NATIVE_TTY_VERSION 1`,
  `OK_NATIVE_JOB_CONTROL_VERSION 1`. `ok/uapi/job_control.h` is not folded into
  `signal.h`: the OS consumer pins `signal.h` and the termination profile line
  for line, and a separate header keeps that pin unchanged.
- `tools/export_sdk.py` validates the two new markers with their UAPI defines
  and the `ok_termios`, `ok_winsize` and `ok_sigaction` records and hashes the
  terminal and job-control headers; the manifest gains `native_tty_profile`
  and `native_job_control_profile`.
- Consumer compatibility: the unmodified OS main refuses the S9a SDK with
  `kernel SDK native console input profile mismatch` (`check_uapi.py`:
  `OK_UAPI_VERSION_MINOR=17 expected=16`). The paired worktree must adopt the
  mirrored consumer changes before it consumes an S9a or later SDK.

## Merge with feat/smp-bringup

Main gained per-CPU run queues, the kernel lock and kernel sections (ABI 80)
while this branch was open. Terminal and job-control work stays under the
kernel lock in `step()` on every CPU. The stop overlay now also gates the run
queues: `enqueue_thread`, stealing (`eligible_on`) and `mark_on_cpu` refuse a
stopped process, `dequeue_thread` discards an entry queued before the stop
(hotplug re-placement drops it), and a stop kicks a thread of the process that
runs on another CPU out of user mode. `test_run_queues.py`
(`test_stopped_processes`, model) covers the gates; removing any one of the
four fails it.

## Paired ObfuscationOS obligations (`feat/getty-login`)

1. Consumer: `tools/kernel_sdk.py` (marker strings and lines, the two new
   validators, UAPI 0.17, manifest keys, required headers), `tools/check_uapi.py`,
   the SDK suites under `tests/tools/` (fixtures, UAPI 0.17, the new
   `test_native_tty_sdk.py`), and the UAPI pins in `lib/okcrt/src/power.c`,
   `tests/native/native_console_lease_probe.c`,
   `tests/native/native_console_control.c` and
   `tests/native_terminal_wrapper_tests.c`.
2. Runtime (since S6/S8): `modules/boot/storage.cpp` calls
   `DeviceFileSystem::attach_terminals(vfs)` before mounting `/dev`; okcrt
   `sigaction()`/`signal()` over native `rt_sigaction` with SIG_DFL and SIG_IGN
   only; oksh (interactive) and login ignore SIGTTOU, SIGTTIN and SIGTSTP.
3. Rewrite the stale `tests/native/native_console_control.c` probe and any
   qualifier that expects EIO for a background console read, POLLERR from
   console poll or ENOTTY for TIOCNOTTY; run the login lane with the paired
   login and oksh.
4. Consume SDK 80 after S9b and merge right after the kernel.

## Receipts

Not packaged yet. At merge they go to ObfuscationEvidence:
`receipts/ObfuscationKernel@<sha7>/feat/tty-jobctl/{hosted-tty,qemu-test-10,sdk-export-10}/`
and `receipts/ObfuscationOS@<sha7>/feat/getty-login/guest-tty-10/`, each with
`result.json`, `commands.txt`, `hashes.json` and `ARCHIVE`, serial logs as
`.log.xz`. The slice runs above were made on worktree copies and are not
receipts.

## Deviations

The 23 deviations from Linux, each with the slice that landed it, are the
table in section 14 of [tty](../modules/tty.md). The main ones: a stop
applies at generation; owed kernel signals are invisible to `sigpending`;
native programs have SIG_DFL/SIG_IGN only and no masks; no ptrace stops,
`kill(-1)` or core bit, and `waitid` only on RV32; no devpts filesystem and 8
PTYs shared with raw pairs; no console vhangup, VTIME or IXON on the console;
VMIN reads are atomic (and a caught signal below VMIN leaves the bytes
buffered); a PTY master cannot be a controlling terminal; pid 1 gets EIO
instead of a restart loop on background access.
