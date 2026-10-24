# Native process termination

The ordinary native syscall dispatcher implements `OK_SYS_KILL` for positive
process IDs. The SDK exports `ok/uapi/signal.h` and advertises the separate
`native_termination_profile` value
`bootcpu-positive-pid-probe-term-kill-deferred-v1`. The C++ module ABI remains 67;
that ABI number alone does not establish this capability. Consumers must select
an explicitly exported SDK containing the new profile and hashed UAPI header.

Signal zero probes an existing user process after the normal directed-signal
credential check. Native recipients support `OK_SIGTERM` (15) and `OK_SIGKILL`
(9); both use the default fatal action. The native C library does not advertise
signal handlers or masking. A native sender addressing a Linux-personality
recipient uses the existing Linux pending-signal mechanism. Linux TERM retains
the recipient's disposition, mask and blocked-wait admission, and may therefore
be caught, remain pending or return `EOPNOTSUPP`. Linux-to-native signalling
through the Linux ABI remains unsupported.

Termination commits at the next serialized boot-CPU service step, after the
sender's syscall return frame has been published and before further user entry
or wait completion. Self-termination therefore cannot destroy the stack or
address space while the dispatcher is publishing the return. Accepted KILL
cannot be downgraded by a later TERM. The existing exit transaction cancels
blocked operations, retires entry stacks and signal owners, clears thread
identity bindings, releases the address space and descriptors, and publishes
the child's wait status. Retirement failure stops further runtime execution.
Native parents receive the ordinary signalled wait status and explicitly reap
the child. Signal zero and fatal signals against an existing zombie succeed
without creating a new termination owner.

The native interface returns `ESRCH` for an absent positive process ID,
`EINVAL` for signals outside 0..64 and `EPERM` for denied credentials. Process
groups, broadcasts, kernel tasks and other nonzero native signals return
`EOPNOTSUPP`. Fatal requests against designated PID 1 return `EPERM`.
This is a bounded process mechanism, not full POSIX signal or shutdown support.
It does not establish SMP delivery or disk consistency. The separate
[PID 1 adoption contract](NATIVE_INIT_ADOPTION.md) describes orphan ownership.

`tests/host/native_signal_production_tests.cpp` exercises the actual dispatcher
and retirement implementation with hosted trap, mapping and copy backends.
Its controls cover permissions, invalid requests, protected PID 1, kernel tasks,
blocked pipe-reader teardown, sibling entry stacks, native wait/reap status,
self-termination, KILL precedence and Linux-recipient KILL. Run the isolated
Debug or Release suite with `scripts/test_native_signal_production.py` and a
fresh `--output` directory; `--sanitize` selects ASan and UBSan. These hosted
checks do not execute user instructions.

The SDK-only OS fixture `tests/native/native_termination_probe.c` executes
blocked-child TERM/KILL, wait status, post-reap ESRCH and self-KILL. The native
service-graph qualifier executes it before starting the dependent service
workflow. Its actual guest result must be recorded separately from hosted
tests and standalone Kernel boot diagnostics.

The kernel administrative API also uses checked completion and adoption for
published user tasks with retained native execution resources. Native-personality
administrative cleanup then reaps immediately, preserving its existing erase
policy; Linux-personality administrative termination stays waitable. Resource-free
model records and unpublished rollback retain their separate cleanup paths.
