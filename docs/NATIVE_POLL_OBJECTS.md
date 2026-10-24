# Native poll objects on owned pages

The serialized boot-CPU native runtime stores actual pending poll/select records
in `ObjectPool<PollWait, NativePagePool, ...>`. The global bounded index holds
PID, TID and a generation handle; it no longer contains the records themselves.
The pool and provider are noncopyable and live for the kernel lifetime. The pool
is constant-initialized and does not depend on hosted static constructors.

`NativePagePool` remains the sole physical-frame owner. User and kernel-object
purposes have separate admission quotas and counters. The object reserve is
computed from the actual record layout and the declared 64-registration bound;
it is eight pages on the currently executed x86_64 layout. User capacity is
therefore 1016 of the 1024 pages on that layout. The reserve is neither an extra
allocator nor additional physical memory. Kernel leases preserve already-mapped
supervisor addresses, including MIPS KSEG aliases. User owns/release/pin/handle
operations reject object pages. Both ELF loading and VM replacement go through
the common purpose-checked mapping boundary, including PROT_NONE backing.

Admission allocates before retaining descriptions or registering the wait ticket.
The bounded index is published only after the complete request and scheduler
ticket exist. Every earlier error drains acquired references, destroys the object
and trims empty pages without replacing the primary syscall error. Completion
claims the exact scheduler ticket before user copy. Cancellation removes index
membership before releasing descriptions and object storage. Scans copy handles
before invoking cleanup and never use a record after release. Object handle
and wait ticket generations protect distinct lifetimes.

Debug native startup writes and reads 64 actual provider-backed records after
hardware initialization, checks capacity/stale handles and returns all pages.
`OK_NATIVE_OBJECTS version=1 scope=kernel` reports this privileged execution.
It is separate from hosted allocator tests and actual user poll execution.
`OK_NATIVE_FRAMES version=2` retains total allocation/release counters and
explicitly adds user/object/free/pending/reserve/user_limit fields. The OS parser
rejects unversioned data and checks purpose sums, quotas and stable reserves.

Validation is separately retained in the workspace prototype receipts: hosted
fault and lifetime tests, all-ten hosted runtime pressure with sanitizer, exact
SDK23 producer and SDK-only consumer, x86_64 native and musl guests, and a live
all-ten Debug/Release regression matrix. The x86_64 user pressure probe runs
eight children, exhausts the shared user quota, wakes real poll calls, checks
contents, releases mappings, reaps children and then reuses memory and poll.
The matrix's terminal receipt, not this document, determines completed profiles.

This is a first bounded object consumer. It does not implement a general heap,
runtime-sized process/VM metadata, per-process allocation policy, reclaim, OOM,
IRQ-safe wait queues, pthread creation, synchronization or SMP. Hosted rejection
before a mapping backend is not a hardware PTE isolation proof. A dedicated
privileged mapping inspection and protected user-load fixture remain required.
