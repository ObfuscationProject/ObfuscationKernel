# Internal pending signal queue

`ok::sched::PendingSignals<MaximumSignal, RealtimeCapacity = 32,
MaximumSequence = UINT64_MAX>` is a bounded, allocation-free queue. The supported
Linux number sets have 64 signals, or 128 for MIPS. Callers serialize access;
the queue itself provides no locking, delivery, scheduler lifetime, permission
checking, usercopy, or SMP mechanism.

## Ownership and tickets

Queue objects cannot be copied or moved. Keep them in a stable runtime-owned
pool, not directly in a value container whose erase operation shifts tasks.
`reset({process, thread, generation})` requires all three fields to be nonzero.
An invalid reset has no effect. A valid reset drains pending work and binds
the supplied owner; it preserves the monotonically increasing ticket counter
even when the owner changes. A process fork needs a separate empty queue;
pending work is not copied with process records.

`peek(blocked, delivery)` leaves the queue unchanged. On failure it also leaves
`delivery` unchanged. A delivery carries owner identity, sequence, complete
owned payload and the originating queue pointer. `consume(delivery)` checks
all of these fields. Tickets obtained before discard, consume, reset, owner
changes or replacement cannot consume later entries. Tickets from another
queue are rejected even if that queue was accidentally given the same owner
identity. Internal array compaction does not change a retained ticket.

Only consume tickets obtained from `peek`; do not synthesize them from fields.
The instance pointer does not replace task generation: reconstruction at the
same address requires a fresh, non-reused external owner generation. A runtime
pool must outlive its tickets. Reset is a drain/rebind operation, not scheduler
deactivation; the runtime must reject enqueues to departed tasks.

`MaximumSequence` is an optional compile-time lifetime budget, used at its
64-bit maximum in production. After the final sequence is allocated, further
new entries return `full` permanently for that object. Reset never restarts
the counter. Coalescing an already-pending standard signal still succeeds,
because it needs no new ticket. The small-budget model exercises this path
without modifying private state or attempting integer wraparound.

## Enqueue and selection

Generic enqueue accepts canonical sender records with `code == 0` (SI_USER)
or `code == -6` (SI_TKILL), selected-range signal numbers and zero child status.
Sender PID/UID values remain verbatim; the runtime constructs authenticated
sender metadata and checks access. The trusted `enqueue_kernel` operation
creates only zero-payload SI_KERNEL records.

SDK31 adds a trusted `enqueue_child_exit(ChildExitStatus, child_pid, child_uid)`
operation. It selects the ABI's SIGCHLD and admits CLD_EXITED or CLD_KILLED with
typed status, nonzero signed-representable PID and copied real UID. Generic
enqueue still rejects positive CLD codes. Typed causes preserve all admitted
MIPS signal numbers independently of packed wait status; this is not a claim of
Linux packed-wait compatibility for signal128. The opt-in private exit path invokes this
operation as described in [NATIVE_CHILD_EXIT_SIGNALS.md](NATIVE_CHILD_EXIT_SIGNALS.md).
See [NATIVE_CHILD_SIGINFO.md](NATIVE_CHILD_SIGINFO.md) for wire encoding.
Other fault, timer, stop/continue and queued-value variants remain unavailable.

Signals 1 through 31 coalesce by signal number and preserve the first pending
record. Signals 32 through the architecture maximum queue independently up
to the shared realtime capacity. Filling realtime storage does not prevent a
new standard signal. Invalid/full/coalesced operations do not allocate a new
sequence or mutate retained payloads.

Selection follows Linux `next_signal`: unblocked ILL/TRAP/BUS/FPE/SEGV/SYS
numbers take precedence among standard signals, even for sender payloads.
MIPS uses BUS=10 and SYS=12; the other supported profiles use BUS=7 and SYS=31.
Within that class, and then within the remaining standard signals, the lowest
number wins. Standard signals precede realtime signals. Realtime signals use
the lowest unblocked number, with FIFO ordering among equal numbers. Blocking
is a filter and never removes work. The caller supplies a sanitized blocked
mask; this queue does not implement signal-mask syscalls.

`discard(signal)` removes all entries for that signal; invalid numbers do
nothing. `pending_mask()` reports all queued numbers, including blocked ones.
`realtime_size()` counts only realtime entries. The default queue stores
28-byte internal entries on i386 and 32-byte entries on the other nine profiles,
without repeating the external owner identity per entry. Its measured size is
1,816 bytes on i386 and 2,072 bytes on the other profiles. SDK31 info records are
20 bytes; external ticket and process-shared wrapper layouts also grow.

## Runtime integration boundary

Frame construction must succeed before `consume` commits removal. A nonleader
exec that needs to retain directed pending work must explicitly transfer or
rebind it under runtime serialization and invalidate old tickets. No implicit
copy, transfer or exec method exists here. Any external transfer must handle
destination coalescing, capacity and sequence exhaustion before retiring
source work. Shared process queues and every directed thread queue remain
separate owners; signal-action discard callbacks must visit all of them.

## Validation

`scripts/test_pending_signals.py --output <new-directory>` retains exact inputs
and generates its own hosted xmake definition. ASan/UBSan tests compare against
an insertion-ordered vector reference over both number sets and realtime
capacities 1, 4 and 32. Dedicated cases cover forged/cross-instance/stale
tickets, reset ABA, coalescing payload retention, invalid metadata, blocking,
Linux standard priority, realtime priority/FIFO, compacted entries and
sequence exhaustion. These are hosted model tests, not signal delivery.

`tests/host/pending_signal_compile.cpp` explicitly instantiates both number
sets with every native cross compiler. Those archives are built, not executed
or exported as an SDK. Action/frame/native runtime qualification remains
separate.
