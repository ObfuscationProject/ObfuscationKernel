# Owned native child waits and private caught signals

Base: frozen Kernel820 inventory
`0f4f4699b1d2a342a7a5937e2b5ae823e7137b34a4bbc442cf79749d02cbdf24`.
This isolated prototype preserves all ten kernel profiles and SDK29 exported
record layouts. It adds no production signal enablement and performs no native
or physical qualification.

The scheduler already supplies parent/child ownership, exit wakeups and reaping.
The missing native foundation was an owned copied continuation: wait4 previously
redispatched from its frame after blocking, and the RV32 adapter wrapped wait4 by
reading a temporary status word back from user memory. That wrapper retained a
UID snapshot across the wait and did not represent a distinct waitid completion.

The additive ChildWaitRequest/common boundary carries selector, output address,
WNOHANG, output format and target endianness. Linux wait4 and RV32 waitid use the
typed boundary; the native syscall marshaller translates its existing wait4.
Other native syscall forwarding remains unchanged. Existing process adapter
coverage moves wait assertions from forwarding to actual common completion.

An ObjectPool-backed ChildWait stores PID/TID, original request generation,
current exact WaitTicket and copied request. No PCB, TCB or user-buffer pointer
survives blocking. Capacity follows entry-stack leases, with object pages
explicitly included in the page-pool reservation. Cancellation retires only its
exact origin/object/ticket, so it cannot complete a replacement wait.

A nonmutating probe chooses a currently waitable child, ECHILD, zero for WNOHANG,
or the only interruptible state: an eligible live child. Output admission is
revalidated. Completion snapshots status/UID immediately before reaping and
re-finds the parent after canonical array compaction. Bytes are encoded for the
request's target ABI. RV32 writes the complete existing siginfo record directly;
there is no intermediate user-buffer read. Checked-copy failure is propagated,
including a failure after the child has already been consumed.

The syscall origin authority adds a distinct ChildBinding. It binds the actual
owned instruction capture, entry lease/image/space/epoch, exact request and wait
registration to wait4 or RV32 waitid. Argument widths remain Linux scalar widths.
No unused MIPS O32 stack arguments are fetched. A ready wake may remain bound;
if a peer consumes that child's status while another eligible child remains,
rearming changes only the exact ticket with a strictly increasing generation.

Linux 6.13.4 kernel/exit.c checks ready children, ECHILD and WNOHANG before
ERESTARTSYS. Musl 1.2.5 waitpid uses wait4 where defined, otherwise waitid on RV32.
Private recipient selection follows that readiness/error precedence. A caught
handler without SA_RESTART receives EINTR. SA_RESTART publishes the original
input frame at the validated syscall instruction, using the existing typed
N64/PPC readiness transaction where required. The old object is retired before
handler setup, and any subsequent execution allocates a fresh request/ticket.
Nested handlers therefore cannot revive an old continuation. Enqueue only
admits an owned blocked recipient; it does not publish results or consume work.
Ignored, blocked, unsupported and stale-origin cases retain ordinary behavior.

Cleanup covers thread exit, group finish, native process release, stale wait
pruning and successful exec retirement. The hosted exec case invokes the actual
retirement helper; it does not execute a full ELF exec. Synthetic capture/copy
backends model user entry and checked copies; they do not establish architecture
execution, physical isolation, SMP safety or independent verification.

## Explicit remaining wait semantics

The retained bootstrap subset supports child exits with a positive PID or any
child (-1), WNOHANG, no rusage, and RV32 P_ALL/P_PID with WEXITED. It does not add
process-group waits, stops/continues, ptrace, WNOWAIT or raw legacy waitpid syscall
numbers. Automatic SIGCHLD, SA_NOCLDWAIT and deferred child autoreaping are supplied
by the later opt-in [child-exit integration](NATIVE_CHILD_EXIT_SIGNALS.md);
child-ready/ECHILD precedence over signal interruption remains unchanged. Musl's waitpid paths are
covered through its selected wait4/waitid ABI.
The current early output-range admission (also on a resumed blocked request),
RV32 requirement for nonnull siginfo and zeroing of the full record are retained
bootstrap behavior. Linux performs wait4's output copy after reaping and permits
null waitid siginfo. Thus this change does not claim complete Linux wait
compatibility or POSIX wait conformance.
