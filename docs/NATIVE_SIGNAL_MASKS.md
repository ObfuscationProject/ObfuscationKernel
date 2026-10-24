# Native per-thread Linux signal masks

The SDK28 working implementation stores a canonical128-bit `sched::SignalMask`
in each native TCB and implements the Linux `rt_sigprocmask` byte contract on all
ten profiles. This is one prerequisite for signal support, not caught-signal
delivery, sigreturn, cancellation, pthread qualification or M3 completion.

The codec processes target unsigned-long words. MIPS O32/N64 use16-byte sets,
how values1/2/3 and SIGSTOP23; other profiles use8-byte sets, values0/1/2 and
SIGSTOP19. SIGKILL9 and SIGSTOP are removed from supplied masks. Unused upper
canonical bits are cleared for64-signal profiles. These syscall records are
smaller than musl's public128-byte sigset_t.

The syscall checks the exact size first. A nonnull set is copied before how is
validated; null set ignores how. The new mask commits before oldset output, so
an output EFAULT retains the new mask. Exact and partial aliasing follow input
copy before output. Current architecture usercopy preflights the whole range:
unlike the tested Linux x86_64 copy_to_user path, a cross-page output fault may
leave no prefix written. This partial-copy side-effect parity remains an
explicit native compatibility gap; the pure operation permits partial output
policies and does not roll back state.

Clone and fork inherit the actual calling TCB's mask. Siblings then mutate
independently. Initial image installation starts empty. Exec snapshots the
actual caller before TCB collapse and supplies that mask to the replacement
image; a nonleader's mask must not be replaced by the former leader's mask.
Precommit failures leave all masks intact. Postcommit fatal ownership failures
remain terminal under the existing entry-stack/VM retention contract.

The older process `SignalState` remains a separate model interface. Native
`rt_sigaction` and `rt_sigreturn` still return ENOSYS, and this slice cannot
deliver a pending signal when a mask is unblocked. Do not report a stored mask
as executed handler support. The RV32 musl1.2.5 action-layout discrepancy must
be resolved explicitly before future signal-action qualification.

Validation is layered: pure codec/operation tests, ten freestanding compiler
instantiations, hosted actual syscall/clone/fork/install tests, a distinct
hosted executed exec-dispatch regression, then separately pinned raw guests
and Linux reference runs. Hosted trap and mapping backends do not prove native
hardware isolation, SMP synchronization or physical platform support.
