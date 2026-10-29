# Native signal runtime ownership

The internal implementation lives in `src/user/signals/`. These are typed
include fragments of `src/user/runtime.cpp`, not test fixtures or independent
translation units. The runtime owns its queues, actions, alternate stacks,
architecture capture state, delivery/sigreturn transactions and blocked syscall
continuations. `private_signal_scheduler_relocation.hpp` declares the internal
runtime hooks used by `src/sched/scheduler.cpp` around process compaction.
Architecture-selected codecs and reusable mechanisms remain in `include/ok`.

The later synchronous wait extension is documented in
[`NATIVE_SIGNAL_WAIT.md`](NATIVE_SIGNAL_WAIT.md). It adds a private owned wait
side table and diagnostics, and appends two public wait reasons while retaining
the ABI29 record layouts. The byte-identity claims below describe the earlier
ownership extraction, not this subsequent extension.

This ownership extraction moves ten implementation headers without changing
their bytes and updates their runtime/scheduler include paths. The existing
`emit_pipe_vectors` diagnostic helper is marked `[[maybe_unused]]` so inherited
hosted profiles without debug test points still compile with `-Werror`.
Public headers, layouts, diagnostic output, behavior and build gates are
unchanged. Hosted tests remain under `tests/host`; their runners snapshot the
new `src` dependencies. The separately gated futex peer fixture remains under
`tests/native` because it drives a test workload rather than signal policy.

## Current enablement and ownership

The ownership extraction below was qualified as a private Debug checkpoint.
The subsequent ordinary enablement uses `OK_NATIVE_SIGNAL_RUNTIME` in both
Debug and Release; `OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE` selects Debug diagnostic
traces only. See [`NATIVE_SIGNAL_PRODUCTION.md`](NATIVE_SIGNAL_PRODUCTION.md) for
the successor capability, administrative completion and remaining limits. The
ten-profile architecture matrix and SDK-only OS consumption rule remain.

The action table has `sched::max_processes` entries (64), keyed by PID and
reclaimed when that PID is absent from the scheduler. Each stable action slot now
also owns the shared queue and lifetime epoch described in
`NATIVE_PROCESS_SIGNALS.md`. Directed pending queues and alternate
stacks have `KernelEntryStackPool::capacity` entries (128), indexed by entry-stack
slot and rebound to `{process, thread, generation}`. A queue rebind drains pending
work and resets its alternate stack. Queue ticket sequences never restart on
rebind. Each queue has 32 realtime entries; standard signals coalesce. The queue
objects remain at stable addresses while process records can move.

Fork copies actions and the alternate stack, with separate empty pending state.
Exec resets caught actions, clears the alternate stack and explicitly transfers
the surviving caller's pending entries when the leader's queue changes. MIPS n64
and PPC state have staged/committed relocation hooks so process compaction does
not silently change captured address-space or frame identity. Admission,
generation/capture checks and continuation completion rely on the serialized
boot-CPU runtime; these arrays do not supply an SMP locking protocol.

## Concrete production prerequisites

- **Lifetime and resource policy.** Retain or replace the bounded pools with
  explicit per-owner limits and fallible allocation. Qualify PID reuse, slot and
  generation exhaustion, realtime queue exhaustion, exec transfer failure and
  simultaneous delivery/exit/compaction under the intended runtime concurrency.
  Some exhausted or lost-owner paths return an errno, while invariant failures
  set `runtime_failure` and stop the whole native service. Production needs an
  explicit fault-containment policy for each of these paths.
- **Fault signals and job control.** A failed handler frame or sigreturn currently
  terminates the process with signal 11. Actual hardware faults are contained by
  immediate process termination in `step`; they are not routed to user fault
  handlers with fault-specific `siginfo`. The queue supports authenticated sender
  records and a trusted zero-payload kernel signal, not a complete tagged fault,
  timer or child payload. Directed sends reject stop-family signals and SIGCONT
  with `EOPNOTSUPP`. Default stop-family delivery returns `unsupported`, which the
  private service treats as failure; default SIGCONT is classified as ignored.
  Implement stopped/runnable group transitions, pending stop/continue interaction,
  parent notifications and wait reporting before advertising job control.
- **Process routing.** The private positive-target `kill` path now uses a stable
  shared process queue; `tkill`/`tgkill` remain directed. Pending queries, ignoring
  dispositions, fork/exec/exit and all qualified wait selectors include shared
  work. Production still requires broader blocked-state qualification, process
  groups/broadcasts, capability/user namespace/LSM permissions and the SIGCONT
  same-session exception. See `NATIVE_PROCESS_SIGNALS.md` for exact boundaries.
- **Action behavior.** Runtime admission accepts the implemented subset
  `SA_SIGINFO`, architecture-supported `SA_RESTORER`, `SA_NODEFER`, `SA_RESETHAND`,
  `SA_ONSTACK` and `SA_RESTART`; it rejects unsupported sanitized flags with
  `EOPNOTSUPP`. The broader action codec UAPI mask is not an implementation claim.
  Restart is qualified separately for pipe read/write, child wait and futex
  continuations; partial writes retain committed progress. Sleep and poll use
  their own interruption rules and temporary-mask restoration. Exit-only SIGCHLD/SA_NOCLDWAIT policy is implemented as described in
  `NATIVE_CHILD_EXIT_SIGNALS.md`; stop/continue events and additional signal
  sources still require their own behavioral implementation.
- **Architecture capability.** Frame codecs require executable handler PCs,
  writable stacks and profile-specific register/mode/alignment checks. Reserved
  trampolines require owned RX mappings with exact code bytes. The current state
  contracts are fixed FXSAVE on x86, FPSIMD on AArch64, ARMv7 D32 VFP, RISC-V D,
  LoongArch scalar FPU, big-endian MIPS o32 FR0/n64 FR1, and PPC e500v2 scalar
  soft-float. They do not advertise arbitrary dynamic extension state; AArch64
  rejects unsupported records such as SVE/SME. MIPS and PPC handler publication
  and return depend on backend capability and capture/readiness attestations.
  MIPS captures syscalls, timers and resolved page faults; a handler whose
  resume state cannot be verified terminates only its process with `SIGSEGV`
  ([MIPS_SIGNAL_RESUME](MIPS_SIGNAL_RESUME.md)).
  Production admission must match actual enabled CPU state, ELF ABI, context
  capture/restore and alternate-stack sizing on every supported profile.

## Validation boundary

The extraction is validated by byte-preserving move checks, exact public-header
identity, inherited hosted actual-runtime regressions and freestanding ordinary
and private runtime/scheduler compilation for all ten architecture profiles.
Object comparison against the exact predecessor checks generated code identity.
Hosted execution with mocked trap/copy/mapping backends is distinct from native
guest execution, standalone kernel boot, SDK consumption and physical hardware.
The integration owner must separately qualify the frozen successor through the
standalone kernel and the architecture/mode-specific OS artifact consumer.
