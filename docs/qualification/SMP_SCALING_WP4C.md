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
  signal stays pending. Every completion of an untimed wait other than the
  signal's holds its bucket (WAKE, WAKE_OP, REQUEUE) or runs while the owner
  is off its CPU (the quiescer's cancel), so the pending check, the
  publication and the interrupt of an untimed wait cannot be split. Only an
  untimed wait restarts, and its restart permit validates only while the
  wait is pending. The expiry of a timed wait holds `task` alone, so a timed
  wait is interrupted first and its EINTR published after: an expiry that
  lands in between finds the wait complete.
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

- **Signal publication under the bucket.** Spec 4.5 cancels the ticket
  first and then returns EINTR. The restart permit of an untimed wait
  validates only while the wait is pending, so an untimed wait is published
  first and interrupted after, all under its bucket; no completion of an
  untimed wait can come between without the bucket. A timed wait (which
  never restarts) follows the spec's order: interrupted first, EINTR
  published after. A timeout landing during a timed wait's publication
  therefore cannot stop the runtime once the step leaves the BKL (WP6).
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
  against wake in both orders and against a timeout, an expiry landing
  during a timed wait's publication (the signal still wins), a failed
  publication,
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

## Review follow-up

- **A user entry whose address space a sibling replaced is retried** (spec
  4.11; WP3's reader protocol). A user entry opens its window and releases
  the BKL; a sibling's mmap, mprotect or munmap (`replace_native_pages`)
  could then mark the old `SpaceTable` slot retiring before the backend's
  `find()`, which refused it, and the step finished the process with status
  127 (`OK_NATIVE enter=failed reason=native address space is unavailable`,
  seen once in the first x86_64 release run at 2 vCPUs). The shared capture
  (`BackendCapture::enter`, `include/ok/user/native_backend_capture.hpp`) now
  compares the entered handle with the process's current one after the
  entry: a failed entry into a space that was replaced meanwhile returns
  `would_block`, and the step treats it like a preemption (no user code ran,
  the frame is unchanged, the next dispatch enters the current space). A
  refusal with the space unchanged stays fatal. The rejection still counts in
  `OK_ENTRY_STACKS rejected=` and `OK_SMP_MM retiring_rejects=`. The x86_64,
  aarch64 and rv64 backend comments no longer claim that a windowed space
  cannot retire. Test: `test_shared_backend_capture.py` (new runner for the
  existing, until now unregistered `tests/host/shared_backend_capture_tests.cpp`,
  which needed a hosted `syscall()` definition): the replaced space yields
  `would_block`, the next activation enters the new space, an unchanged
  space keeps the backend's status; it fails without the fix.
- **A woken futex wait no longer blocks a sibling's mmap, mprotect or
  munmap.** A wait a WAKE or its timeout completed keeps its syscall-origin
  binding until its owner's resume produces the result (before this package
  the waker retired it). `replace_native_pages` preflights every live origin
  of the process (`stage_space`, then `commit_space`), and that check
  (`current_owner`) admitted only a pending futex wait, so in that window a
  sibling's address-space change failed with `syscall origin space preflight
  lost owner` (EACCES). musl's pthread_join then leaked the joined thread's
  stack and a later pthread_create met EAGAIN. Found by the guest futex
  lanes at 2 vCPUs on rv64 (2 of 12 runs; a kernel with a debug print showed
  the failing munmap), not reproduced at d7fde31 (9 of 9 clean). The
  preflight now also admits a completed, unproduced futex wait
  (`completed_futex_wait`); `current_bound_wait`, and so a restart, still
  admits only a pending one. The hosted model of the origin table
  (`test_native_futex_origin.py`, an inputs runner the suite never runs) no
  longer builds: it omits the uapi headers and several sources, and with
  those added its fixture's origin `prepare` fails. The guest lanes are the
  test of this fix.
- **Signal against expiry of a timed wait** (above): closed for WP6 instead
  of left as a note. Test in `native_futex_bucket_tests.cpp`: an expiry run
  from inside the publication of a timed wait; it fails with the old order.
- **Guest futex lanes on ten profiles.** The xmake test points cover the
  algorithms only, and distro10 boots take no futex bucket (`OK_SMP_LOCKS
  futex=0:0`). The paired OS change (the package's `os.patch`) adds
  `scripts/test_futex_lanes.py`, which boots the native distro once and runs
  eight static-musl probes in turn, then requires every `OK_SMP_KERNEL_CHECKS`
  counter 0, `max_lock_depth <= 12` and, in debug, futex bucket acquisitions:
  - the new `tests/musl/futex_buckets.c`: wait/wake, a timeout that leaves no
    queued wait, a timed-out waiter queued ahead of an untimed one against
    WAKE(1) (the untimed one must wake), CMP_REQUEUE between two words and
    signal against wake (the wait returns 0 exactly when the wake counted
    it, EINTR otherwise);
  - the existing `native_futex_signal.c` (EINTR, SA_RESTART restarts, timed
    EINTR and timeouts of raw FUTEX_WAIT traps; 76 cases per form and ABI),
    `native_futex_wake_operation.c`, `native_shared_futex.c`,
    `native_threads.c` (clear-TID exit wakes), `native_robust_lists.c`,
    `private_futex.c` and `futex_smp_atomic.c`.

  ppc's libc printf needs a soft-float libgcc the toolchain does not ship, so
  on ppc the three printf probes run through `ppc_*.c` wrappers that define
  the double helpers as traps (`tests/musl/ppc_integer_printf.h`), and
  `futex_buckets.c` writes its line without stdio. The shared ppc musl tree
  had failed its build without the pinned Clang frontend; the lanes use one
  rebuilt with it, and mips64 links the pinned binary128 soft-fp runtime
  (both under the scratch `waveB/WP4c/musl`).
- **`futexgate.sh`** (spec section 7, gate 4) does not exist in the scratch
  tree or any package's logs; the futex probe gate is the OS
  `test_futex_smp_atomic.py` (gate.sh `PROBE=futex_smp_atomic`) plus the
  futex lanes above at 2 and 4 vCPUs.
- **docs/modules/smp.md** is not edited by packages; the text describing the
  buckets and the FUTEX_WAKE behaviour change is proposed in the package
  report for the integration (WP7c rewrites that document).

## Validation

All QEMU runs are TCG on the SysToolchain QEMU 10.2 with private OS copies
of b8dceae (the WP2, WP4a and WP4b OS patches are empty) plus this package's
OS patch. Unless named, results are at f29bb53 (the last kernel source
change; later commits change documentation only).

- **Hosted suite** (`--toolchain-root`, `--jobs 8`): 175 passed, 0 failed,
  59 not run. It includes the new `test_shared_backend_capture.py` and
  `test_native_futex_bucket.py` on ten profiles, `test_native_futex`,
  `test_native_futex_operations`, `test_native_futex_signal`,
  `test_native_threads`, `test_robust_futex` and `test_futex_word`. An earlier
  run (at ec412ef, host load near 30) failed the interleaving-coverage
  checks of `test_wait_hash` (i386, `checked > 0 && woken > 0`) and
  `test_wait_park_race` (`before_park > 0 && after_park > 0`), as the
  implementer's run had; both passed when rerun alone, and this package does
  not touch them.
- **Kernel lanes**: debug `xmake test` at `-smp 1` on all ten profiles and at
  2 and 4 on x86_64, aarch64 and rv64: 16/16 `OK_TEST_PASS`.
- **distro10**: all ten profiles `OK_USERLAND acceptance=pass`.
- **Gates at 2 vCPUs** on x86_64, aarch64 and rv64: `--user-hotplug` debug,
  `PROBE=futex_smp_atomic`, MODE=release (five runs on x86_64),
  `--smp_hotplug_test=y --require-hotplug`, `SECTIONS=off`, and the CPU sysfs
  probe on x86_64: 20/20 pass; every `OK_SMP_KERNEL_CHECKS` counter 0,
  `unqueued=0`, `max_lock_depth` 5 in debug and 0 in release.
- **Guest futex lanes** (`test_futex_lanes.py`, eight probes per boot):
  ten of ten profiles at `-smp 1` (ppc through its `ppc_` wrappers); at 2
  and 4 vCPUs on x86_64, aarch64 and rv64: 6/6 pass.
  Each debug run takes about 15000 futex bucket acquisitions with
  `lock_violations=0`, `class_asserts=0` and `max_lock_depth` 6. Repeats of
  the rv64 lane at 2 vCPUs: 12/12 pass with no thread-create retry.
- **Before the origin fix** (ec412ef: the entry retry and the signal
  order): the same 2-vCPU gate matrix passed
  except one x86_64 `--smp_hotplug_test=y` run, which failed a CPU restart
  with `OK_SMP_HOTPLUG_FAILURE ... reason="x86 secondary TSC was not
  synchronized"`. That kernel-side failure also appears in the WP4d and WP5
  runs of this gate (packages without these changes); three reruns pass.
  One of those release runs (x86_64 rel2r3) recorded `retiring_rejects=1`
  and `OK_ENTRY_STACKS rejected=1` with no `enter=failed` and passed: a
  replaced space was retried in a guest. The x86_64 release gate at 2
  vCPUs passed 10 of 10 runs across the two rounds.
- **A/B for the origin fix** (rv64 at 2 vCPUs, `futex_buckets` probe only):
  d7fde31 12/12 clean (no retry); a8d96ae (the implementer's head) 10/10
  pass with three
  thread-create retries; 02dcc61 (before the origin fix) 2 failures in
  12 runs (EAGAIN from pthread_create); f29bb53 12/12 clean.
- **Not reproduced as package failures**: the boot-time `POSIX_CHECK failed:
  child_status(child) == 0 errno=16` of the OS self-test, which ended one
  x86_64 distro boot and two lane boots (rv64, aarch64) before any probe ran.
  It appears in waveA integration and WP4b and WP4d logs.
- **Not run**: release `xmake test` lanes, gates at 4 vCPUs after the review
  fixes (the implementer's 4-vCPU runs predate them), the `legacy_select=y`
  lane, and the `OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE` and
  `qualify_private_futex_peers.py` lanes. Their OS-side evidence tools pin
  older SDK ABIs (`native_signal_production_evidence.py` admits ABI 31) and
  the peer fixture's accepted execution profile is x86_64; the futex lanes
  above cover the same WAIT, WAKE, timeout and signal behaviour on all ten
  profiles.
