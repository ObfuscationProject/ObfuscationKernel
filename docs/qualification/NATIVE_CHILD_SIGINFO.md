# Typed child siginfo prerequisite

This codec stores and encodes child completion payloads. The opt-in runtime
integration in [NATIVE_CHILD_EXIT_SIGNALS.md](NATIVE_CHILD_EXIT_SIGNALS.md) now
generates automatic SIGCHLD and implements deferred child autoreaping.
Production signals remain disabled.

`sched::ChildExitStatus` constructs a typed normal or signaled cause independently
of packed wait status. Generic sender enqueue still rejects positive CLD codes.
Trusted `enqueue_child_exit` assigns the target's SIGCHLD and validates cause,
nonzero signed-representable child PID and copied real UID. A blocked shared
queue retains the first standard-signal payload when further notifications
coalesce; it never uses the lossy realtime fallback for child information.

`SignalInfoCodec<Profile>` creates a zero-padded 128-byte target-endian siginfo
record. All realtime frame families and synchronous wait use it. Legacy frame
admission recognizes the typed cause while retaining each legacy frame layout.
Delivery bridges preserve child_status. Existing frame-specific SI_KERNEL
restrictions are unchanged. Invalid encoding leaves the output buffer unchanged;
frame and user-copy transactions retain their existing ownership rules.

The payload supplies CLD_EXITED or CLD_KILLED, group PID, real UID and typed status.
User/system CPU fields remain explicitly unimplemented zeros. No CLD_DUMPED,
stop/continue, ptrace, timer or queued-value payload is advertised. MIPS uses
SIGCHLD 18 and swapped code/errno offsets; the other profiles use SIGCHLD 17.
The typed MIPS signal-128 representation is not a packed-wait compatibility claim.

SDK/module ABI 31 is required: PendingSignalInfo and frame Info records now have
five fields (20 bytes), so pending queue entries and their public instantiated
owners grow. Consumers must use the exact architecture/mode SDK; ABI30 artifacts
must be rejected. Frame wire layouts remain fixed. All ten kernel profiles and
the four OS distribution profiles remain part of their respective matrices.

Run `scripts/test_native_child_siginfo.py --output <fresh-path> --sanitize` for
hosted payload, encoding, admission, coalescing, stale-ticket and exhaustion
models. Such tests are not native execution, process isolation, or automatic
SIGCHLD acceptance. The private runtime supplies parent-thread
lineage, fork rollback suppression, checked deferred reaping and a version-selected
leader notification cause. Its native qualification remains separate from codec
model evidence.

The selected x86_64 Linux 6.13.4 reference reports a retired leader's status in
SIGCHLD and the final group cause in wait results. A newer WSL2 Linux 6.18 host
reports the group cause in both. Those recorded version-specific observations do
not establish authenticated source-to-binary provenance or other-profile Linux
execution. The private lifecycle selects the recorded 6.13.4 behavior.
