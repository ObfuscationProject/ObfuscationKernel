# Private synchronous signal waits

The existing private Debug signal service implements `rt_sigsuspend`,
`rt_sigtimedwait` and the available `rt_sigtimedwait_time64` syscall numbers on
all ten architecture profiles. The ordinary bootstrap still returns ENOSYS.
Old non-RT suspend, new signal sources, job control, SMP and production signal
enablement remain separate work.

`src/user/signals/wait_codec.hpp` copies masks and time inputs using the target
ABI. Exact kernel sigset size is required (8 bytes, 16 on both MIPS profiles).
Size validation precedes set copy, timeout copy and timeout-content validation.
Null timeout is infinite; zero timeout polls once. Timeouts are input-only and
valid huge durations saturate at Linux KTIME_MAX. rv32 exposes only time64.

Synchronous receive selects eligible directed work before shared work using
existing owner-validated queue tickets. Selection uses the copied wanted set,
independent of the caller's live delivery mask and installed disposition.
KILL and STOP are excluded. The receive consumes exactly once **before** optional
128-byte siginfo copyout: EFAULT does not restore the consumed occurrence. An
empty poll/timeout does not access info. The private zero-filled output codec
supports SI_USER, SI_TKILL and trusted zero-payload SI_KERNEL records. SDK31 adds
the shared typed CLD_EXITED/CLD_KILLED encoder described in
[NATIVE_CHILD_SIGINFO.md](NATIVE_CHILD_SIGINFO.md); the opt-in native exit path
now generates those records through [NATIVE_CHILD_EXIT_SIGNALS.md](NATIVE_CHILD_EXIT_SIGNALS.md). Queued values, timers and other child event types remain
unavailable, and the private child CPU-time fields are explicitly zero.

Deferred waits use `SignalWaitState`, a bounded private side table keyed by
entry-stack lease, PID/TID and a nonwrapping slot epoch. A scheduler WaitTicket
owns completion; the record holds the copied request and private absolute
deadline. No user pointer is dereferenced by a sender, and no queue/TCB pointer
is kept across scheduling. `signal_wait` and `signal_suspend` are appended
public `ThreadWaitReason` values 9 and 8. Existing values 0–7 and the u8 layout
remain unchanged. This changes SDK source/artifact identity but not ABI29
record layouts: re-export exact architecture/mode SDK artifacts when integrating.

During timedwait, `real_blocked` retains the original mask while the live mask
temporarily unblocks wanted signals. Both directed generation and process kill
consider this original for ignored-signal retention. Process kill still checks
the named task, including nonleader aliases. Completion restores and clears
real_blocked before any unrelated handler, timeout or output error.

Suspend instead saves the original mask for the eventual signal frame and
installs the requested live mask. The handler observes the temporary mask plus
its normal action/self blocking; sigreturn restores the original (or the user's
edited ucontext). Poll/suspend saved masks are not real_blocked. Ignored-only
work is consumed while suspend stays blocked with its temporary mask. For
timedwait, unrelated originally-unblocked ignored work instead interrupts the
raw syscall with EINTR; normal return delivery drains it without a handler.
Both default-ignore and explicit SIG_IGN follow these rules. SA_RESTART does
not restart either raw syscall.
A suspend frame deferred by architecture readiness never returns to the user
program before a handler commits. If a peer removes the pending handler with
SIG_IGN, the logical suspend rearms with a new ticket and the same masks.
Its already-staged EINTR stays unobservable and is not republished through the
one-shot PPC completion proof.

The serialized completion order is explicit. Group SIGKILL runs first. A
previously committed exact cancellation leaves queued work untouched and gives
a still-live owner EINTR. Otherwise a matching timedwait occurrence wins over
an elapsed deadline. Without a match, elapsed deadline gives EAGAIN before an
unrelated signal at that observation; normal signal delivery follows under the
restored original mask. With no elapsed deadline, an actionable outside signal
interrupts with EINTR. Linux's exact unrelated-signal/timer race depends on its
winning wake cause; this is the private runtime's deterministic serialization.
A generic ready wake alone consumes nothing: it rearms the copied logical
request with a new ticket and the same absolute deadline.

Retirement restores/discards private state before lease/address-space release.
Fork/clone never inherit an active wait; nonleader exec discards departed
records while preserving its existing queue-transfer contract. Failed exec
does not clear state. Native step accounts for both finite and infinite waits.

Private diagnostics carry copied ownership rather than rereading user inputs:

- `OK_NATIVE_SIGNAL_WAIT_ARM` identifies PID/TID, logical epoch, scheduler wait,
  kind, syscall/resume PC, canonical original input addresses/size, copied wanted
  and original masks, active temporary mask, absolute deadline and live count.
- `OK_NATIVE_SIGNAL_WAIT_COMPLETE` gives outcome, signed result, pending sender
  metadata, optional info destination and `consumed`. Only receives consume in
  this completion. `published=0 staged=1` on suspend means EINTR is staged but
  remains unobservable until the handler commits; timed/cancelled completion
  uses `published=1 staged=0` after restoring and clearing the owned wait.
- `OK_NATIVE_SIGNAL_WAIT_REARM` keeps the logical epoch and copied request,
  identifies old/new wait generations and the ready/delivery cause, and carries
  `published=0`. No user return occurs at this event.
- `OK_NATIVE_SIGNAL_WAIT_COMMIT` follows successful handler frame/readiness
  publication, before clearing saved ownership, and identifies the delivered
  signal with `published=1 result=-4`.
- `OK_NATIVE_SIGNAL_WAIT_RETIRE` identifies thread/group/exec cleanup before
  entry-stack or address-space release, with `published=0`. This is retirement
  of owned state, not an invented cancellation return to a departed frame.

Mask fields use two numeric u64 words; the upper word is zero outside the MIPS
128-bit domain. `live` is the observed active-record count after the associated
transition; `live_before` explicitly precedes commit/retirement clearing.
Immediate receive/poll has no deferred owner and emits no ARM/COMPLETE pair.
An ignored suspend drain emits no terminal completion. Hosted tests and native
guest observations must demonstrate those behaviors separately.

Validation evidence distinguishes actual hosted runtime execution (mock
trap/usercopy/mapping backends), isolated target-ABI codec models, freestanding
compilation, actual Linux reference user programs, and actual Obfuscation guest
execution. None is substituted for another. Root owns independent review,
standalone SDK and OS artifact-consumer/native qualification before promotion.
