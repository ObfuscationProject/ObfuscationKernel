# MIPS handler resume after resolved faults

Slice of `fix/mips-signal-resume` (Kernel issue #8). On mips (o32) and mips64
(n64) the native runtime stopped the whole guest the first time a user signal
handler was due after the thread had taken a page fault, which copy-on-write
fork makes routine: BusyBox `ash` halted when its `SIGCHLD` handler was due.

## Cause

The MIPS readiness state that admits a handler frame is built from backend
captures. Only syscall and timer traps published one; a page-fault trap revoked
the thread's readiness even when `resolve_native_write` then resolved the
copy-on-write fault and resumed the same frame. The next handler delivery found
the readiness `unverified`, and the runtime treated that as a kernel invariant
failure (`OK_TEST_FAIL native-runtime=private MIPS handler resume unverified`,
`n64` on mips64) and stopped.

## Change

- A page fault, like a timer interrupt, leaves EPC at an instruction that has
  not completed (the branch when `Cause.BD` is set). Both backends now publish
  that capture for page faults too; the capture kinds and readiness states
  `timer_non_bd`/`timer_bd` are renamed `interrupted_non_bd`/`interrupted_bd`.
  The runtime consumes the capture for a page fault taken on the owned entry
  stack, so a resolved fault keeps the thread's readiness. A delay-slot fault
  defers the handler to the next capture exactly as a delay-slot timer does.
  Faults that are not resolved still terminate the process with `SIGSEGV`.
- A handler gate refusal (`unverified`) now terminates only that process with
  `SIGSEGV`, as Linux `force_sigsegv` does for an unusable frame, and prints
  `OK_NATIVE_SIGNAL_FRAME_REFUSED pid=<pid> signal=11`. The runtime keeps
  running. Missing readiness owners remain runtime invariant failures.
- `native_signal_delivery_mips_o32_tests.cpp` expected `SA_NOCLDSTOP` to be
  refused; the delivery policy has accepted `SA_NOCLDSTOP` and `SA_NOCLDWAIT`
  since exit-only child signals landed. The check now uses an unassigned o32
  flag bit and also checks that the two child flags deliver.

The enumerator rename changes exported headers but not their binary layout; no
ObfuscationOS source names these enumerators.

## Tests

Hosted models (`scope=model`):
- `scripts/test_native_signal_mips_o32.py`: delivery (the stale check fixed),
  readiness (an unverified observation never promotes readiness; a revoked
  lease makes the gate refuse; an `interrupted_non_bd` capture proceeds) and ELF
  admission.
- `scripts/test_native_signal_mips64_n64.py`: the tracker publishes page-fault
  captures; a delay-slot fault defers and a plain fault is ready.

QEMU execution (`scope=hardware`, SysToolchain 0.1.0, QEMU 10.2.0), ObfuscationOS
`main` with the WT1.5 BusyBox 1.37.0 userland and this kernel's debug SDK:

| Check | mips | mips64 |
| --- | --- | --- |
| `scripts/test_musl_shell.py`, `sh-acceptance.sh` | 35/35 | 35/35 |
| BusyBox `testsuite/runtest`, `testsuite-gate.txt` (13 applets, 151 cases) | 133 pass, the 9 reviewed failures, 9 untested | same |
| `xmake test` (standalone debug kernel) | pass | pass |

Before the change both profiles stopped after 3–5 sh checks. The results now
match the other eight profiles in ObfuscationOS `docs/qualification/MUSL_USERLAND.md`.

## Limits

PPC keeps its own readiness model and its gate refusal still stops the
runtime; it has not shown this failure. Signals that interrupt a delay-slot
fault wait for the next syscall or timer capture.

## Receipts

Validated at module ABI 77 on `feat/linux-personality` `6968183`, which merges the
four P1 review branches, with ObfuscationOS `feat/linux-personality` (guest runs at
`8cb051a`, tip `9d9bae5`): ObfuscationEvidence `receipts/ObfuscationKernel@6968183/feat/linux-personality/{kernel-ten-profile}` and `receipts/ObfuscationOS@9d9bae5/feat/linux-personality/userland-gate` (mips and mips64 pass like the other eight).
