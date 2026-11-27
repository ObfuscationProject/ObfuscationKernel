# SMP scaling WP4c: futex buckets

Futex package of the smp-scaling work (spec section 4.5, with the futex
parts of 4.4). Futex waits queue on 64 hash buckets, each with its own lock,
instead of one table serialized by the big kernel lock (BKL); wakers
complete waits but never write the waiter's frame or free its record, and
the waiter's own resume produces the result. The step and the futex syscall
still run under the BKL; the bucket locks (rank 42, 43 through `lock_as`)
nest above it and below `task`. Baseline: d7fde31 (WP4b after its review).
The module ABI is not bumped.

## What it adds

- **Buckets** (`src/user/runtime/futex_buckets.hpp`). `FutexBuckets<Wait,
  Lock, 64>`: each bucket is a `FineLock<futex_bucket>` over a FIFO list
  linked through the waits themselves and a `u64` queue sequence mutated
  under the lock. A key's bucket is a Fibonacci hash of a digest of exactly
  the fields `same_futex_key` compares (domain, sharing, then pid and address,
  or frame owner, index, generation and word offset). A pair is taken in
  index order, the higher bucket through
  `lock_as<futex_bucket_second>`; an equal pair once (`PairGuard`).
- **The `bucket` word**. Each `FutexWait` names the bucket that lists it, or
  none. It changes only with that bucket held (a move holds both) and is
  loaded by the owner without a lock and checked again under the lock it
  names (`lock_linked`). An unlink publishes none last, with release order;
  a waker reads the ticket before it unlinks and touches the record no more,
  so the owner may retire the record as soon as it sees none. A requeue's
  `move` never publishes none.
- **WAIT** (`futex`). The index slot, the shared-frame pin and the record
  are taken first. Then, under the bucket: one atomic load of the word
  through `with_user_word` (its own kernel window, no fault), the compare
  (EAGAIN), the deadline check (ETIMEDOUT), the arm under `task`
  (`register_current_wait`), the heap key of a timed wait and the queueing
  at the tail with the next sequence. A word that is not accessible now is
  resolved without the bucket (`uaccess::user_range`) and tried again; after
  `max_retries` (4) attempts the last one holds the process's `mm` (`mm` ->
  bucket -> `task`), so it queues or meets a genuine EFAULT.
- **WAKE**. Under the bucket, each matching wait in queue order is unlinked
  whatever happens next and completed as ready if it is still pending;
  only those count, up to the limit. A wait that timed out, was interrupted
  or cancelled is skipped, so it no longer absorbs a wake (Linux
  `futex_wake_mark`). The CPUs the completions queued waiters on are kicked
  after the unlock (`complete_wait(..., kicks)` and `notify_cpus`).
- **WAKE_OP and CMP_REQUEUE** hold the caller's `mm` across the key
  resolutions, the word resolution (copy-on-write included for WAKE_OP's
  second word) and both buckets; the read-modify-write or the compare runs on
  the frame under both buckets (real atomics; plain operations on i386, which
  runs one CPU). Before taking `mm` the caller waits for its process's kernel
  sections (`drain_if_pinned`), which walk the page records a resolution
  rewrites; with the BKL held no new one is admitted until the call returns.
  REQUEUE chooses pending waits only (`SchedulerState::wait_pending` under
  `task`): an over wait leaves the queue uncounted. All new pins precede the
  wakes and the moves, as before; moved waits join the destination queue
  last, numbered by its sequence; a requeue onto the same key keeps the
  wait's place and number.
- **Timeout**. A timed WAIT has a heap key (`DeadlineAction::timeout`); its
  expiry completes the wait under `task` only and leaves it queued.
- **resume_futex** (spec 4.4), from `resume_owned_wait` after the pick: the
  owner unlinks a wait a timeout or a cancel left queued, checks that the
  record is its continuation's (a record whose ticket the thread no longer
  holds is retired and produces nothing), validates the captured syscall
  origin (`validate_completed_futex`, new) and maps the completion: ready 0,
  timeout -110, any other -4. The result goes into the frame in the same
  step, as for polls and sleeps.
- **Signal** (`private_futex_signal_complete` through
  `interrupt_futex_wait`). Under the wait's bucket it leaves the queue; if it
  is still pending the interrupted call's outcome is published (EINTR, or the
  restart) and the ticket is interrupted. A wait some other completion
  reached first is left to the resume, which produces that result, and the
  signal stays pending.
- **Teardown**. `cancel_futex` takes the record off its queue first (exit,
  exec, thread exit, a lost origin binding).
- **Deleted**: `expire_futex` (the per-step futex scan), `finish_futex`
  (the waker's write of the owner's result and frame), the snapshot,
  selection and revalidation of the old global queue
  (`snapshot_futex_queue`, `selected_futex_wait`, `pending_futex_wait`) and
  the global `futex_queue_sequence`. The peer fixture's control completion
  uses `wake_futex_object`.
- Evidence: the bucket acquisitions count in `OK_SMP_LOCKS futex=`.

## Deviations from the specification

- **Signal publication under the bucket.** The futex syscall origin
  validates only while the wait is pending (the restart permit relies on
  it), so the signal path publishes the outcome under the bucket after a
  pending check and then interrupts. With the BKL held no timeout expires in
  between; once the step leaves the BKL (WP6) a timeout that completes the
  wait between the check and the interrupt reports `futex signal lost
  current wait` and stops the runtime, as the old order did. WP6 must close
  that window (let the restart permit accept an interrupted completion, or
  have the expiry of a futex key take the bucket).
- **`validate_completed_futex`** (new in `src/user/native_syscall_origin.hpp`)
  admits a completed futex wait whose owner has not produced its result;
  `validate_bound` keeps admitting pending waits only, so a completed wait
  still cannot restart (`native_futex_origin_tests` unchanged).
- **The `futex_waits` identity index stays** a 128-entry table searched by
  (pid, tid); spec 3.8 re-indexes it by lease slot. It is read and written by
  the owner, the quiescer and the signal path, all under the BKL.
- **Error precedence of WAIT**: the duplicate-wait check, the index, the pin
  and the record now come before the compare, so with the record pool
  exhausted a WAIT whose word differs returns ENOMEM, not EAGAIN. An EAGAIN
  allocates and frees a record.
- **Robust-list retirement and `clear_child_tid`** still reach the word
  through `uaccess::user_word` without holding `mm` (under the BKL, as
  before); they move under `mm` with the exit work (WP7b).
- **REQUEUE's choice of pending waits** is not in the specification; it
  keeps an over wait from taking a wake or a transfer slot, like WAKE.

## Tests

- New `test_native_futex_bucket.py` (`tests/host/native_futex_bucket_tests.cpp`,
  ten profiles, the actual runtime and scheduler built with
  `OK_HOSTED_LOCK_MODEL` and `OK_LOCK_DEBUG`): a timed-out waiter against
  WAKE(1) (the second waiter wakes; the first resumes with -110), signal
  against wake in both orders and against a timeout, a failed publication,
  requeue across buckets (FIFO behind the destination's waiter, destination
  sequence, owner unlink from the destination), an equal-bucket pair for
  WAKE_OP and CMP_REQUEUE and a different-bucket pair through `lock_as`, with
  no lock-order report, `class_asserts`, `held_asserts` or `queue_asserts`.
  Host-thread races on `FutexBuckets` with real `futex_bucket` locks (20000
  rounds each): compare-and-queue against a waker that changes the word
  (no lost wake), a requeue moving a wait while its owner unlinks it, and a
  signal's interrupt against WAKE(1) (exactly one completes the first wait;
  when the signal wins the wake goes to the second).
- Adapted to owner-produced results (a wake no longer writes the frame or
  frees the record; each test runs the waiter's resume as the step does):
  `native_futex_tests` (timeouts through the heap key; a timed-out waiter
  skipped by WAKE; stale records retired by the resume without touching the
  frame or a replacement ticket), `native_futex_requeue_tests` (bucket
  sequences instead of the global one; an unpin refused at the owner's
  resume rather than in the wake; a stale index found at the resume;
  `snapshot_ticket_substitution`, which tested the deleted snapshot, is
  replaced by `requeue_skips_completed`), `native_futex_wake_operation_tests`
  and `native_futex_signal_tests` (two new recipient modes: a real WAKE
  before the probed dispatch, and the signal before a WAKE), and
  `native_threads_tests` (the clear-TID waiter's result is produced at its
  resume).

## Validation

Kernel sources (`src`, `include`) are those of 421adfa throughout; a3645ee
changes `tests/host` only. All QEMU runs are TCG on the SysToolchain QEMU
10.2, with private OS copies of b8dceae (the WP2, WP4a and WP4b OS patches are
empty; this package has no OS change).

- **Hosted suite** (`--toolchain-root`, `--jobs 8`, at a3645ee): 172 passed,
  2 failed, 59 not run. The new `test_native_futex_bucket.py` passes on ten
  profiles, as do `test_native_futex`, `test_native_futex_operations`,
  `test_native_futex_signal`, `test_native_threads`, `test_robust_futex`,
  `test_futex_word`, `test_scheduler_waits` and `test_native_exec_eviction`.
  The two failures are interleaving-coverage checks of unchanged race models,
  run at a host load average near 38: `test_wait_hash` (i386,
  `checked > 0 && woken > 0`) passes on a rerun; `test_wait_park_race`
  (`before_park > 0 && after_park > 0`) failed once more alone, then passed
  twice, and fails the same check at d7fde31 under the same load.
- **Kernel lanes**: debug `xmake test` at `-smp 1` on all ten profiles and at
  2 and 4 on x86_64, aarch64 and rv64: 16/16 `OK_TEST_PASS`.
- **distro10**: all ten profiles `OK_USERLAND acceptance=pass`.
- **Gates** on x86_64, aarch64 and rv64: `--user-hotplug` debug at 2 and 4
  vCPUs, release at 2 and 4, `PROBE=futex_smp_atomic` at 2 and 4,
  `--smp_hotplug_test=y --require-hotplug` at 2, `SECTIONS=off` at 2, and the
  CPU sysfs probe at 4 on x86_64 and at 1 on arm32. Every
  `OK_SMP_KERNEL_CHECKS` counter is 0, `unqueued=0`, `max_lock_depth` is 5 in
  debug and 0 in release.
- **One kernel-side failure, not from this package**: the first x86_64
  release run at 2 vCPUs ended `SMP_STRESS_FAIL reason=worker-4-round-0-line-10127`
  (the worker exited 127) after `OK_NATIVE enter=failed reason=native address
  space is unavailable`, with `retiring_rejects=1`. A user entry opens its
  window and releases the kernel lock; a sibling's mmap, mprotect or munmap
  (`replace_native_pages`) then marks the old space retiring, `find()`
  rejects it and the step finishes the process. The entry and the retiring
  state come from WP3 and are not touched here. Four reruns of that gate
  pass with every counter 0.
