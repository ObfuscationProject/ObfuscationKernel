# Private futex implementation contract

This change introduces Linux FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE through the
existing checked user-copy boundary. It is a prerequisite for the planned full
thread substrate, not a declaration that pthreads or the selected POSIX profile
are complete. Shared futexes, bitset/requeue, robust ownership, PI and signal
restart remain explicit implementation work and are refused at this boundary.

A key is a monotonically allocated live process ID plus aligned user virtual word
address. It remains valid across unrelated mapping rebuilds; hardware table
slots/generations are not address-space identity. Fork uses a different process
key. Unmapping and remapping the same address does not change the private key,
as in Linux mm-plus-VA semantics. The plan P07 stronger reused-VA isolation
wording remains unresolved; it is not claimed by this bounded contract. PID
keys require revisiting before CLONE_VM across different processes is enabled.
Successful exec and process teardown drain old waiters before retiring the image. Private wait does not retain a data-page pointer or pin; user memory is
copied only at admission. User code owns synchronization and mapping lifetime.

A page-backed object stores each wait key, exact scheduler ticket and optional
monotonic deadline. A bounded 64-entry index stores handles only. Futex and poll
each contribute their maximum backing-page count to the global Kernel reserve
so both quotas can coexist when user pages are exhausted. Allocation, quota or
registration failure must leave the caller running, existing tickets and counts
unchanged, and release all unpublished object storage. Capacity is currently a
bootstrap limit; runtime-sized VM/objects and OOM policies remain required.

The boot CPU serializes value comparison, object admission and scheduler wait
registration without re-entering userspace. No other user thread can change the
word between the comparison and publication. This is the explicit current
compare-and-block guarantee; introducing IRQ/SMP producers requires locking and
memory-ordering work before enabling them. No new virtual dispatch is added.

Wake completes the exact process/address key up to the signed positive count.
For legacy Linux compatibility, zero or negative counts wake one matching waiter
(the source loop increments before comparing). An empty queue returns zero.
Timeout/wake/cancel compete for the same generation ticket; only the winner may
write the saved return value. Membership is removed before object destruction,
and stale owners/tickets are drained without completing replacement waits.
Runtime completion writes 0 for wake and ETIMEDOUT for deadline expiry. Cancellation
is terminal for the old ticket; process exit/exec does not resume the old image.
External scheduler cancellation of the same live ticket returns EINTR exactly
once and clears the pending syscall. It never repeats WAIT; a replacement ticket
and its continuation are untouched. This is not signal delivery or restart.

Linux time32/time64 and byte order are decoded per profile. Timeout validation
and copying happen at syscall admission. A relative timeout becomes an absolute
monotonic deadline before copying the futex word; copy time cannot extend it.
Wait compares a copied 32-bit word;
mismatch yields EAGAIN even for an already expired timeout. Pointer alignment,
address bounds and caller identity are checked before publication. Unsupported
operations return ENOSYS rather than a fabricated wake/synchronization success.

Hosted runtime tests, ten-profile target compilation and actual user-mode
execution are separate qualification gates. This document specifies the work;
no full conformance, physical or SMP result is asserted by its presence.

The exported C++ request and wait-reason change advances the exact Kernel SDK
and OS consumer gate to ABI24. The native C UAPI remains0.6.0. ETIMEDOUT is
reference-generated for all ten profiles:145 for MIPS/O32 and MIPS/N64,110 for
the other profiles. Source references pin Linux6.13.4 and musl1.2.5.

The static-musl fixture qualifies mismatch, finite/zero timeout, time layouts,
copy errors, empty WAKE and distinct fork keys. Peer wake tests currently use
real runtime/scheduler code with hosted trap/copy backends. The fixture's
integer-only diagnostic writer does not qualify musl printf/quad-float support;
MIPS64's existing printf link dependency remains a separate toolchain gap.
