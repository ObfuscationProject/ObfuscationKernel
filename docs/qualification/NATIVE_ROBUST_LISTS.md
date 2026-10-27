# Serialized native robust-list cleanup

SDK27 adds Linux-personality `set_robust_list` and bounded `get_robust_list`,
and executes native robust-futex retirement on the existing CPU0 service path.
All ten architecture profiles remain selected at compile time. This is not
complete pthread, PI, signal, shared-memory or SMP support.

## ABI and task lifetime

Syscall numbers are generated from pinned Linux 6.13.4 and musl 1.2.5 inputs.
The head comprises three target native words: next, signed futex offset and
pending. Codecs preserve target pointer width, endian order and modular address
arithmetic; checked architecture copies validate the resulting address.

Set validates only a length of three target words, then records the caller's
pointer without accessing it. Null unregisters. Get selects the current TCB for
TID zero, or a visible Linux-personality user TCB for a positive signed 32-bit
TID. Retired nonleaders are no longer visible; the unreaped leader remains
visible with its cleared registration. Get writes target size_t length before
the target head pointer. Aliasing and second-store faults preserve that order.

Same-group get queries are supported. Cross-group queries return EPERM even for
modeled root, an explicit Linux compatibility gap: saved credentials,
dumpability, user namespaces, ptrace capabilities and security hooks are not
implemented. Missing targets return ESRCH before output access.

The checked-copy path accepts unaligned get output buffers when their bytes
are writable. Exact Linux parity for unaligned MIPS/PPC outputs still needs
selected CPU/configuration execution: scalar put_user instructions can enter
alignment emulation paths, so neither unconditional success nor unconditional
EFAULT follows from the store instruction alone. Aligned ABI probes do not
close this separate edge-case gate.

Clone and fork start with an empty registration. Allocatable task IDs stop at
0x3fffffff so neither robust state bit can alias an owner ID. Individual exit,
group exit and external process release consume each registration before its
clear-child-TID handling and before releasing the owned address space. Invalid
lists do not prevent task teardown.

Exec captures value records before scheduler collapse. Reversible failures do
not mutate robust registrations, words or waiters. Every committed path,
including fatal entry-stack corruption, consumes the saved records against the
still-owned old VM. Siblings use their original TIDs; the caller runs last with
the process TID, following Linux de_thread identity transfer. Thus an old
nonleader-owned word can remain unmatched. The new image starts unregistered.
The caller is the final old-MM user in the currently supported clone model, so
its clear-child-TID pointer is reset without a user store.

Cleanup follows scheduler commit within one serialized boundary, including
close-on-exec descriptor release. This ordering is bounded to the current CPU0,
private-mapping implementation. Before observable file-release callbacks,
cross-group shared MMs or SMP are supported, split exec into validated
preparation and destructive commit with retirement hooks at the sibling-exit
and caller-MM-release points.

## Walker and memory contract

The walker follows Linux's next-link read, current-owner handling, fault and
pending ordering and limits traversal to 2048 nodes. A listed pending entry is
processed once after traversal. Malformed early termination skips pending.
Matching owners become `(word & WAITERS) | OWNER_DIED`. Non-PI words with WAITERS
wake at most one same-process shared-operation waiter. A non-PI pending owner
zero attempts wake without a store. Private-operation waiters are a separate
key domain. Tagged PI words can be marked, but there is no PI-state teardown or
priority inheritance implementation.

`SerializedRobustAccess` requires checked reads, compare/exchange32 and wake.
The native adapter uses full writable-range checks and endian-aware copies.
These are indivisible relative to native users because every user is stopped
on CPU0; they are not hardware atomic operations for SMP. Anonymous pages are
eager. Under copy-on-write fork, the writable-range check itself resolves a page
still shared after fork, so the following copy writes a private frame. Future
demand paging and transient atomic faults require fault-in/retry within the
access policy. Terminal copy faults
currently end this best-effort walk.

## Validation layers

`test_robust_futex.py` exercises codec/traversal models. Freestanding compilation
checks the ten target profiles without execution. `test_native_robust.py`
executes actual adapters, scheduler, wait queues and retirement with hosted
mapping/trap backends. OS raw-clone probes and independent Linux references
have separate source, SDK, compiler, binary and execution receipts. Only their
actual passing guest receipts establish native syscall execution; none alone
closes M3 or the M0–M12 release gates.
