# Bounded Linux sleep and monotonic resolution

Linux nanosleep, clock_nanosleep, clock_nanosleep_time32/time64 and monotonic
clock_getres/time32/time64 enter the real boot-CPU runtime deadline mechanism.
All ten Linux personality profiles remain present. The generated syscall tables
cross-check pinned musl 1.2.5 against inspected Linux 6.13.4 source tables;
ProfileData's new operation arrays require SDK ABI 12. Native UAPI remains 0.4.0.

The syscall boundary copies one target-endian timespec. Legacy records use two
target longs. Kernel time64 uses two 64-bit fields and ignores the high tv_nsec
padding on 32-bit ABIs, independently of byte order. Seconds must be nonnegative;
nanoseconds must lie in [0, 1000000000). Clock IDs and flags are 32-bit scalars on
LP64. Only TIMER_ABSTIME affects raw clock_nanosleep flags, as in Linux. Unsupported
clock selection rejects before copying the request. clock_getres accepts a null
result and writes only the selected target record size otherwise.

Relative nanosleep and CLOCK_REALTIME durations use monotonic elapsed time.
CLOCK_MONOTONIC supports relative and absolute sleep. Absolute CLOCK_REALTIME,
CPU clocks and other clocks return EINVAL in this bounded profile. No realtime
epoch, suspend accounting or clock adjustment is claimed. Linux interval/deadline
arithmetic saturates at KTIME_MAX, including Linux's whole-second saturation
threshold. Native sleep retains its original invalid/overflow behavior.

Both native and Linux calls use native::begin_sleep with a copied typed request,
explicit owner and current monotonic time. It arms the process leader's existing
generation-bearing DeadlineWait, completes an expired deadline immediately, or
blocks the owner. Failed blocking cancels admission. A runtime step completes the
saved frame through its actual personality once; it does not redispatch the call
or recopy a changed request. Exec/exit/teardown cancel the registration. Success
leaves remainder memory unchanged, including an invalid remainder address.

This is still a single native leader per process on the serialized boot CPU.
Signals, EINTR/restart, cancellation, CPU clocks, generic event wait queues,
SMP synchronization and an interrupt-driven idle loop remain unimplemented.
The existing outer runtime scan can run other processes and GUI services while
sleepers wait; it is not the workstation no-idle-polling exit gate.

Hosted tests exercise all ten ABI codecs against the common runtime mechanism,
clock/copy failures, frozen request/generation behavior, saturation and resolution.
A separate hosted runtime-step test checks deadline completion without replay,
encoded Linux success and teardown. The debug-kernel ABI test preserves a labeled
arithmetic model check. These are distinct from the musl QEMU probe's measured
relative/absolute elapsed time and child progress while the parent sleeps.
