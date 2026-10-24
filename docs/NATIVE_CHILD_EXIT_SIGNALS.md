# Private native child exit signals

The native runtime connects group completion to typed process-shared SIGCHLD
and checked deferred autoreaping. Its initial qualification was private Debug.
The ordinary successor uses `OK_NATIVE_SIGNAL_RUNTIME` in both build modes,
with Debug traces selected separately. This remains an exit-only serialized
boot-CPU subset, not full POSIX signals or a general SMP lifecycle implementation.
All ten architecture profiles retain their existing frame/delivery paths.

The final live thread causes one group completion. A caught SIGCHLD action
admits a shared notification. Default SIGCHLD admits it only when the named
parent creator/replacement thread blocks that signal or its active sigtimedwait
retains it in real_blocked. Explicit SIG_IGN suppresses automatic generation
even when blocked and detaches the completed child. SA_NOCLDWAIT also detaches
the child but preserves caught/default admission. Default ignore without that
flag retains the ordinary waitable zombie. Actions are sampled at completion;
later disposition changes do not sweep old zombies. SA_NOCLDSTOP is retained as
an exit-only no-op. Retaining either flag on another signal has no child effect.

The parent owner slot carries an epoch, and the child records the actual fork
caller TID. A departing parent thread rebinds its children to the lowest live
native sibling TID. Successful exec rebinds children to the canonical surviving
TID and clears the old leader exit cause. Failed exec leaves lineage unchanged.
Native-personality children with private lineage also rebind, because they can
later exec Linux. A native-personality parent keeps native child waiting without
Linux notification or autoreaping when its child execs musl. Parent0 is the
existing service reaper, never an invented Linux init/subreaper recipient.

The selected Linux6.13.4 x86_64 reference reports an already-exited leader's
cause in SIGCHLD independently of the final group wait cause. The private owner
therefore saves a raw leader exit cause; final raw exit, exit_group or fatal
signal cannot overwrite it. Wait status selection is unchanged. Child real UID
is copied by value, and standard coalescing preserves the first pending payload,
including a preexisting SI_USER SIGCHLD. Wait/reap never consumes that payload.
Zero siginfo clock fields remain placeholders, not CPU accounting.

`src/user/signals/child_exit_lifecycle.hpp` owns preparation, typed commit,
lineage inheritance/rebinding and the fail-fast completion transaction.
`runtime.cpp` snapshots the decision before retiring private state and invokes
commit only after successful cleanup and scheduler zombie transition. Created
fork rollback bypasses notification/autoreap and retains its explicit cleanup
wait. Commit re-finds owners by ID and epoch; no borrowed pointer survives it.
Autoreap only detaches the scalar parent link. Physical PCB/TCB compaction occurs
at the original early/end service reaper boundaries, with observable failures.
Cleanup, transition, enqueue, detach and checked reap failures stop the private
service. Failure barriers prevent later user entry or reaping after such a fault.

Administrative `Kernel::kill_process` now sends published Linux tasks through
the same checked completion transaction with SIGKILL, preserving a waitable
zombie or deferred autoreap according to parent policy. It reports cleanup
failure before scheduler reaping or UI callbacks. Non-Linux administrative
cleanup and unpublished rollback retain their previous policy. Synchronization,
real CPU accounting, general orphan adoption, stop/continue/job control and
MIPS signal128 wait status remain open. See `NATIVE_SIGNAL_PRODUCTION.md` for
the successor qualification boundary.

SDK ABI31 is retained: private owner growth changes no exported layout, and the
checked scheduler additions introduce nonvirtual methods and a new progress
record without changing existing exported record sizes. Consumers still require
the exact newly exported architecture/mode headers and libraries.

Tests remain distinct: `test_native_child_exit_lifecycle.py` exercises actual
helper/scheduler C++ under ten hosted architecture selections;
`test_native_child_exit_runtime.py` exercises actual x86 runtime wiring with
mock trap/copy/mapping backends. Neither proves native execution. Standalone
builds/boots and SDK-consuming OS QEMU campaigns remain separate integration
requirements. The historical scheduler, signal-wait and process-signal tests
are preserved.


## Native integration qualification

The SDK31 lifecycle integration executed both policy and lineage probes twice
on each of the ten architecture profiles. Policy qualification covers 360
isolated cases: 800 process completions, 160 physical deferred reaps, 300
notifications and 60 coalesced notifications. Lineage qualification covers 140
isolated cases, including actual armed sigtimedwait and ppoll ordering. Source
and native diagnostic joins bind child identity, parent thread, selected leader
notification cause and final wait cause. Pipeline waits may already be ready;
the lineage collector explicitly validates every emitted blocking wait without
claiming that all successful wait calls emit diagnostic pairs.

Fresh qualification also exports twenty architecture/mode-specific SDKs and
boots ten private standalone kernels. The ordinary signals-off x86 Debug SDK
has a separate build/boot; ordinary OS consumers link five images and boot ten
times across the four distribution architectures. OS builds consume only the
exported artifacts. These checks preserve the existing public layout values;
matching SDK31 library and header identity is still required.

The additional hosted runtime audit retains 164 ASan/UBSan assertions on actual
exec and exit code with mocked mapping, copying and trap backends. It covers
canonical entry-stack reuse, full-pool failure, lineage replacement and terminal
cleanup failures. Those are hosted checks, not native pressure/failure execution.
The original failed collector attempts and later strict replays remain separate
receipts. No replay is counted as another guest execution.

Production flags, CPU accounting, stopped/continued child events, general
orphan adoption, SMP and physical platform acceptance remain outside this
exit-only integration. It does not close M3 or the M0-M12 workstation plan.
