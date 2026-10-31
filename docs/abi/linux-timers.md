# Linux interval timers

The Linux personality implements `ITIMER_REAL` (`setitimer`, `getitimer`,
`alarm`) and the POSIX timer calls `timer_create`, `timer_settime`
(`timer_settime32`, `timer_settime64`), `timer_gettime` (`timer_gettime32`,
`timer_gettime64`), `timer_getoverrun` and `timer_delete` on all ten profiles.
`scripts/generate_linux_abi.py` cross-checks every number against Linux 6.13.4;
`alarm` exists only on i386, x86_64, mips, mips64 and ppc, the time32 variants
only on the 32-bit profiles that have them, and rv32 has only time64. Native
processes do not take these calls.

## Contract

The reference is Linux 6.13.4 `kernel/time/itimer.c` and `posix-timers.c`.

- Wire records (`ok/abi/linux_timer.hpp`): `itimerval` is four target longs,
  interval first, with microseconds; `itimerspec` is the legacy two-long or the
  `__kernel_timespec` form, the time64 form ignoring the high `tv_nsec` half on
  32-bit ABIs. Seconds must be nonnegative and the sub-second field below one
  second, else `EINVAL`. Values saturate at KTIME_MAX. `timer_create` reads the
  whole 64-byte `sigevent`; its value word round-trips in target byte order.
- Order of checks follows Linux: copy-in (`EFAULT`) before validation
  (`EINVAL`) before timer lookup (`EINVAL`); `setitimer` and `timer_settime`
  apply the new setting before copying the old one out, so a bad old pointer
  returns `EFAULT` with the timer already changed.
- `timer_create` takes `CLOCK_REALTIME` and `CLOCK_MONOTONIC`, the clocks
  `clock_gettime` serves, and `SIGEV_SIGNAL`, `SIGEV_NONE`, `SIGEV_THREAD`
  (process-directed, as the kernel sees it) and `SIGEV_THREAD_ID` with a live
  thread of the caller. A NULL `sigevent` is `SIGALRM` with the timer id as
  `sival_int`. Ids count up per process from 0 and wrap at `INT_MAX`; an id is
  consumed even when the `sigevent` is then refused or the id cannot be copied
  out. Eight POSIX timers per process; the next `timer_create` fails `EAGAIN`.
- Bases: `ITIMER_REAL` and `CLOCK_MONOTONIC` timers run on the monotonic clock,
  as does a relative `CLOCK_REALTIME` timer (`common_hrtimer_arm`). A
  `TIMER_ABSTIME` `CLOCK_REALTIME` timer runs on the wall clock, so
  `clock_settime` moves its expiry; while the wall clock is unavailable it
  waits, and arming one returns the clock's error.
- Signals: `ITIMER_REAL` sends `SIGALRM` as `SI_KERNEL` to the process
  (`it_real_fn`). A POSIX timer queues one record with `si_code` `SI_TIMER`
  (-2; -3 on MIPS, where -2 is `SI_ASYNCIO`), `si_timerid`, `si_overrun` and
  `si_value`, to the process or its `SIGEV_THREAD_ID` thread. Admission is
  that of `kill` and `tgkill`: an ignored, unblocked signal is dropped, and an
  expiry no thread can take yet (every unblocked thread in a wait that does
  not admit signals) stays owed and is retried on the next step.
- Rearm: an interval timer rearms when its signal is dequeued, forwarding its
  expiry on the original grid past that time; the intervals it skipped are the
  `si_overrun` of that signal and `timer_getoverrun` afterwards (clamped to
  `INT_MAX`). While a signal is queued no second one is queued. `getitimer`
  reports an expired `ITIMER_REAL` as inactive until then; `timer_gettime`
  forwards a signalled or `SIGEV_NONE` interval timer past now and counts what
  it skipped. A one-shot timer past its expiry but not yet served reports 1 ns
  (`timer_gettime`) or 1 µs (`getitimer`).
- `timer_settime` and `timer_delete` drop the record the earlier setting
  queued (Linux 6.13 drops it at dequeue via the signal sequence); `setitimer`
  leaves a queued `SIGALRM` alone. An ignored interval timer parks until its
  signal is unignored, then queues with the overrun it accumulated; an ignored
  `ITIMER_REAL` stops, keeping its interval for `getitimer`. A timer whose
  `SIGEV_THREAD_ID` target exited stays signalled and never rearms.
- `fork` inherits no timer. `exec` deletes the POSIX timers and their queued
  records and keeps `ITIMER_REAL`; the id sequence continues. Process exit
  deletes everything.

## Mechanism

`ok/time/interval_timer.hpp` holds one timer's state with no signal state;
`src/user/runtime/timers.cpp` owns a side table per process and serves the
calls. Each runtime step reads the monotonic clock (and the wall clock for
realtime-based timers), expires due timers, queues their signals through
`private_timer_signal`, refreshes the overrun of a queued record, and rearms a
timer whose record left its queue since the last step. Records live in the
existing pending-signal queues (`PendingSignalInfo` gained the timer id,
overrun and value), so delivery, `sigwaitinfo`, `sigsuspend` and EINTR from a
sleep are the existing paths. The kernel reports `OK_TIMERS scope=hardware
armed=… expired=… signalled=… rearmed=… retried=…` with its interrupt report
once a timer was armed.

## Limits

- Expiry granularity is the runtime step, so the 100 Hz tick when idle; no
  one-shot hardware deadline is programmed.
- `ITIMER_VIRTUAL`, `ITIMER_PROF` and the CPU-time clocks return `EINVAL`: no
  per-thread CPU-time accounting exists. `CLOCK_BOOTTIME`, `CLOCK_TAI` and the
  alarm clocks are not served, like `clock_gettime`.
- A legacy-numbered timer signal holds one queue slot: a second timer (or a
  `kill`) with the same number waits until the first is dequeued, where Linux
  queues both records.
- Job-control signals from a timer are dropped, as job control is not
  implemented.
- `sigpending` stops showing a record that `timer_settime`/`timer_delete`
  dropped; Linux keeps it visible until the dequeue discards it.
