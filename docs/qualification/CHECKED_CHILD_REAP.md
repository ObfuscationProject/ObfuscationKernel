# Checked child detachment and orphan reaping

These scheduler mechanisms are a prerequisite for private native child-exit
policy. No native runtime caller opts into them yet. Their existence does not
implement automatic SIGCHLD, SIG_IGN autoreap, SA_NOCLDWAIT or a Linux reparenting
policy. No production feature gate changes.

`detach_completed_child_to_reaper(parent, child)` runs in a serialized scheduler
operation after completion has been published. It requires distinct nonzero
identities, a present non-exited parent, the child's matching parent backlink,
exactly one parent child-list entry, and no competing forward link. It accepts
only zombie/exited children whose descendants have already been reparented.
Every thread must be exited with native execution, syscall/wait/deadline and
entry-stack ownership retired. The child cannot remain selected on a CPU.
Native VM/pages, file slots, VM-area/mapping records and cwd-node ownership must
already be released.

Validation is read-only. Success removes one scalar child-list entry and sets
the child's parent to the service reaper0. It does not allocate, invoke a
compaction hook, erase a process, move a PCB/TCB, change exit/wait status, or wake
threads. Existing completion already wakes child-specific and any-child waiters;
a future serialized policy must finish detachment before permitting scheduling.
The retained zombie is physically erased only at a later service boundary.
An already-detached child and every malformed or incompletely retired state
fail without changing scheduler storage.

`reap_orphans_checked(progress)` is an explicit alternative to the existing
`reap_orphans()` service-boundary operation. It resets `OrphanReapProgress`,
visits parent0 zombie/exited records in canonical table order and stops at the
first `reap_process` failure. It returns that exact Status and failed PID. The
`reaped` count describes erased PCB records; if a relocation hook fails after
erase, the count includes that record and `failed_after_erase` is true. The PID
is then absent and must not be blindly retried. A pre-erase failure leaves the
failed record available for inspection; later orphans remain untouched.

Both reaping APIs invalidate canonical process/thread pointers on erase and
therefore require the caller to use a service boundary without outstanding
borrows. The existing unchecked method retains its prior skip-failure behavior
and count semantics. Native runtime call sites remain unchanged. Physical
retirement and publication still require their existing architecture-specific
hooks; this helper supplies no SMP synchronization or hardware execution proof.

The hosted test calls the actual scheduler implementation under each of the ten
architecture selections. It verifies child-list ordering, all PCB/TCB addresses,
wait status and wake tickets, read-only rejection of malformed relationships and
undrained resources, and checked-reaper partial progress. Existing private
relocation-hook boundaries supply test-only pre/post-erase failures. These are
hosted mechanism tests, not native guest or production-autoreap evidence.
