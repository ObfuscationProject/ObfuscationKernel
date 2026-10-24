# Ordinary bounded native signal runtime

`OK_NATIVE_SIGNAL_RUNTIME` selects the existing boot-CPU signal service in both
ordinary xmake Debug and Release targets, across all ten architecture profiles.
`OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE` is now an optional Debug evidence setting.
Private signal/wait/child traces remain behind that setting plus test points;
Release defines neither. Historical private fixture runners retain compatibility
through `native_signal_config.hpp`; this does not manufacture Release test points.

This successor is an implementation candidate pending root integration and
fresh native qualification. Prior private guest receipts establish the prior
checkpoint only. Hosted tests exercise actual runtime code with mocked traps,
user copies and mappings; they are not QEMU or physical execution.

## Preserved runtime contract

Process/thread queues, blocked masks, dispositions, altstack, architecture frame
and sigreturn checks, synchronous waits, supported wait interruption/restart,
SIGPIPE and child exit policy retain their existing implementation. No signal
numbers or supported action flags change. Existing unsupported process-group,
stop/continue and unqualified blocked-state sends retain EOPNOTSUPP. Hardware
fault handling remains fatal SIGSEGV without catchable fault-specific siginfo.
CPU-time fields are still zero. No general SMP, dynamic extension-state or POSIX
conformance claim is added by making the bounded implementation available.

MIPS o32/n64 and PPC backend issuers, capture and lease access now follow the
ordinary runtime gate. MIPS64 integer-only early admission receives the same
definition in assembly in both modes, before hard-float compiled code. The
original precise CPU/FPU/ELF admissions remain. Ordinary early MIPS64 platform
messages have a distinct `N64_SIGNAL_PLATFORM_*` prefix; private Debug evidence
retains `N64_PRIVATE_EARLY_*`. These checks are functional platform admission,
not test points. Runtime/scheduler compaction preflight and commit also follow
the same gate, preserving owned captured frames when PCBs relocate.

## Administrative termination

The internal `src/user/native_lifecycle.hpp` declares the checked serialized
`terminate_native_process` operation. It accepts an existing published Linux
group, a valid fatal signal, its active runtime Kernel owner, retained native
address space and live threads with valid inactive entry leases. No native
entry may be active. Missing, created, wrong-personality, partially released,
wrong-service and repeated-completion requests return Status without changing
shared runtime failure. A prior service failure is propagated.

`Kernel::kill_process` uses SIGKILL through that operation for published Linux
tasks. The common `finish` transaction prepares parent policy, cancels waits,
retires stacks/signal ownership/TIDs/VM, commits group exit and publishes at most
one child event. Normal parent policy leaves a waitable zombie; explicit IGN or
NOCLDWAIT only detaches it for the checked service reaper. A duplicate request
returns `already_exists` and cannot reap or re-notify. Parent0 continues to be
the service reaper. The original parent creator/replacement lineage and saved
leader notification cause remain distinct from the final group wait cause.

Failure during retirement stops native service before transition, event or
subsequent entry. Failure is returned to the administrative caller; it does not
fall through to unconditional scheduler reaping. Non-Linux tasks and unpublished
rollback retain their old cleanup policy, with a checked runtime-status barrier
before immediate scheduler cleanup. Low-level direct scheduler deletion remains
an infrastructure operation, not a published native-user termination API.

Existing UI/extension callbacks run after committed administrative completion.
A callback failure can therefore be returned after the child has already exited;
no rollback or second event is implied. Retrying a completed Linux PID returns
`already_exists`. Callers must distinguish lifecycle completion from downstream
UI/service bookkeeping failure.

## SDK contract

The SDK native C++ ABI remains 31 subject to unchanged public layout measurements
and integration review. New exports carry mandatory `native_signal_profile`,
derived from the actual runtime define: `bounded-bootcpu-v1` when enabled and
`disabled` otherwise. The exporter rejects Release diagnostics, inconsistent
private configuration and mismatched MIPS64 early assembly/runtime settings
before replacing an existing artifact directory. The coordinated OS successor
requires the supported profile plus the matching runtime define. Old manifests
are preserved; missing metadata is not silently inferred as new capability.

## Validation

`test_native_signal_production.py --mode debug|release [--sanitize]` runs actual
hosted runtime administrative policy, duplicate completion, leader cause,
parent0 reclamation, invalid boundary, cleanup-failure and ordinary caught-signal
checks. The Release selection carries `OK_NATIVE_SIGNAL_RUNTIME` but neither
private diagnostics nor test points. `test_native_child_exit_runtime.py` retains
its original private lifecycle cases, sharing only its setup helpers with the
new suite. `tests/host/test_native_signal_export.py` checks all ten capability
rows and rejects contradictory metadata before publication.

Integration must freshly build and boot standalone Kernel and consume exact
architecture/mode SDKs from OS; old artifact replay cannot replace those runs.
Physical hardware and manual desktop acceptance remain separately deferred.
