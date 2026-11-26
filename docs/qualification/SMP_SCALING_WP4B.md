# SMP scaling WP4b: wait channels, deadlines, owner results

Wait package of the smp-scaling work (spec sections 4.3, 4.4, 4.6 and 4.10).
A wake now visits the waits linked on its channel instead of every thread,
deadlines live in one indexed heap instead of per-step scans, and a poll or
a sleep produces its own result on its owner's CPU. The step still runs under
the big kernel lock (BKL); the wait_hash (rank 50) and deadline (rank 54)
locks nest above it. Baseline: 12c2d2d (WP4a after its review). The module
ABI is not bumped.

## What it adds

- **Wait channels** (`include/ok/sched/wait_hash.hpp`). `WaitHash<Buckets,
  Entries, Lock>`: 64 buckets, each a `wait_hash` spin lock with a FIFO list,
  over a fixed entry pool (1024 entries on x86_64, aarch64 and rv64, 256 on
  the other profiles). An entry (32 bytes) holds the channel, the waiter's
  TCB slot, tid and the low 32 bits of its registration generation, and a
  link in its owner's chain. A pool slot is claimed by a word exchange and
  released by a store, so no lock guards the pool and i386 needs no
  compare-exchange. A waker only detaches an entry; its owner frees every
  entry it inserted, so an entry index stays the owner's until it lets go.
- **A1 arms**. `register_current_wait` first drops what the thread's previous
  wait left linked (owner unlink), arms under the `task` lock, and for a
  channel reason (child, pipe read/write, console read, PTY read/write, job
  control) links the wait on `(reason, key)` before returning, so while the
  caller still holds the lock of the condition it checked (today the BKL).
  A thread slot's entries form a chain through the entries
  (`SchedulerState::wait_links_` keeps its head and count, at most 16).
- **A2 wakes**. `wake_waiters` keeps its signature and its call sites: under
  the bucket lock it detaches up to 16 matching entries, releases the lock,
  and completes each one's wait as ready (`task`, then `run_queue`) if the
  slot still holds that tid with that pending generation; it takes again
  while the bucket still holds a match.
- **Owner unlink**. Once a wait is over (a take, or a timeout, probe, cancel
  or a poll's own completion that leaves entries linked), its owner walks
  its chain, detaches what is still linked and frees every entry: right
  after the pick (`settle_wait_links`), at its next arm, and for a thread
  being retired at exit, exec, thread exit, slot reuse and `cancel_poll`. A
  free thread slot keeps no entry and no deadline.
- **Exhaustion**. With no entry (or no link slot) left, the arm still
  succeeds: the wait counts `unqueued` (`OK_SMP_WAKE`) and gets a retry
  deadline one 10 ms tick out, whose expiry completes it as ready so that its
  owner re-evaluates.
- **A3 polls** (`src/user/runtime/wait.cpp`). `poll_channels` maps each
  watched descriptor to the channels whose wakes cover what `poll_ready`
  consults: pipe and UNIX stream buffers (`pipe_read` on the read side,
  `pipe_write` on the write side, peer closes included), a UNIX listener's
  accept queue (a new `pipe_read` key, woken when a connection is queued),
  PTY endpoints, the console input (`console_read` on the poll's console
  epoch; losing the console now also wakes that channel), UDP sockets and TCP
  streams. `arm_poll` registers the wait, links every channel, arms the
  timeout and only then evaluates readiness again; readiness found then
  completes the poll at once.
- **UDP and TCP wake sites**. The UDP receive-change pass wakes
  `(udp_receive, socket slot)` for every socket whose readiness moved, and
  every socket's channel when the executor's readiness moved (transmit
  credit, link failure). `TcpStreamWaits::poll` wakes `(tcp_stream,
  endpoint)` for every endpoint whose readiness moved. The socket wait-queue
  notices for polls (`watch_poll_sockets`, `mark_poll_dirty`,
  `watch_udp_socket`, the `socket_only`/`dirty` flags) are gone.
- **Deadlines** (`DeadlineHeap<576>` in `include/ok/sched/deadline.hpp`). An
  indexed binary min-heap over 512 thread keys and 64 process keys, with a
  position index, so it cannot overflow and set, remove and pop are O(log n).
  The scheduler keeps it under the `deadline` lock with the tid, generation
  (low 32 bits) and action of each thread key and an `earliest` mirror (an atomic word on the 64-bit
  profiles). A wait has one deadline (`arm_wait_deadline` keeps the earlier
  of two arms); `complete_wait` removes it under the `task` lock. Expiry
  (`expire_deadlines`, every step on every CPU) pops up to 32 due keys per
  batch under the lock and acts after releasing it: `timeout` completes the
  wait as timed out, `retry` as ready, `probe` sets the probe of a pending
  wait and queues it. Lateness feeds `OK_SMP_WAKE deadline_late_max_us`.
- **Thread keys**: deadline sleep, the poll timeout, the PTY VTIME timer
  (`expire_tty_timers` is gone), the exhaustion retry, and a timed signal
  wait (a probe key: the owner's probed dispatch finds the timeout).
- **Process keys**: each process's interval timers publish their next
  service time (`timers_next`): the earliest armed expiry, or now while a
  timer waits for its signal's dequeue, owes or parks a signal, or runs on
  the wall clock. The step services only the due processes
  (`private_timer_service_due`); the all-process pass stays for models.
- **Owner results** (spec 4.4). `resume_owned_wait` runs right after the
  pick, before anything decodes the frame. A completed poll re-runs
  `poll_ready`: readiness or its timeout writes the output and the result;
  otherwise the readiness that woke it was consumed by a peer and it re-arms
  (Linux `do_poll`; a documented change). A timed-out sleep returns 0. A
  result lets the thread enter user mode in the same step; a re-armed poll
  parks again. A wait cancelled or interrupted some other way drops its
  object and the call is dispatched again, as before. Wakers no longer write
  any owner field: the poll scan (completion, `revents`, result) and the TCB
  deadline scan are deleted, and `complete_wait` no longer cancels the
  owner's sleep record.
- **Poll audit** (spec 4.10). Every fourth audit (every 1024th CPU 0 pass)
  probes and queues every parked poll waiter; its probed dispatch re-runs
  `poll_ready`, and readiness no wake reported completes it and counts
  `poll_audit_ready`.

## Deviations from the specification

- **UDP receive and TCP stream waits stay on their per-socket wait queues.**
  Their completion depends on the queue's `may_wait` evaluation of observed
  sequences, and the Linux UDP receive owner validates its capture there; a
  plain channel wake would complete them without it. Their waits are
  therefore not channel reasons; the UDP and TCP channels serve polls. The
  per-step `scan_*_udp_receives` passes stay (they carry the UDP wake site).
- **The owner frees; no generations or handles.** The spec has whoever
  unlinks an entry bump its generation and free it, and owners keep up to 16
  `{bucket, index, generation}` handles in the TCB. Here wakers only detach
  and the owner frees, so the owner's entries chain through the entries
  themselves and need no generation; a detached entry stays claimed until
  its owner's next dispatch (bounded: the owner is runnable). The chain head
  lives in a scheduler table by thread slot, not in the TCB, because clone
  and fork copy TCBs. This keeps the per-scheduler cost at 48 KB: a debug
  kernel holds thirteen schedulers (the kernel's and twelve test models),
  and the first layout (153 KB each) left posix-check's 4 MB mmap without
  frames at 4 vCPUs on x86_64 (`free_frames` 2052 at the baseline, 1563
  then, 1892 now).
- **No free-list lock**: pool slots are claimed with a word exchange.
- **`native_deadline` stays** as the owner's sleep record (it supplies the
  remaining time of an interrupted Linux sleep). It is no longer written by
  wakers; its owner settles it at its resume, an interrupted sleep's
  completion cancels it, and a sleep arm drops a record whose wait ended.
- **Process keys poll while attention is needed.** Linux rearms an interval
  timer when its signal is dequeued; nothing reports the dequeue, so a
  process with a signalled, owed or parked timer, or a TIMER_ABSTIME
  CLOCK_REALTIME one, keeps its key due and is serviced each step as before.
  `clock_settime` therefore needs no recompute.
- **Prune scans stay.** `prune_pipe_reads`, `prune_pipe_writes`,
  `prune_pty_waits` and `prune_child_waits` are unchanged (the last still
  completes child waits whose child became reportable); `prune_poll_waits` is
  new and only drops polls whose owner is gone or whose wait was replaced.
  The per-step signal probe (`probe_interrupted_waits`) and `expire_futex`
  stay for WP4d and WP4c.
- **Unwatched sources**: a TCP stream without an endpoint has no channel; a
  poll on it counts `unqueued` and re-evaluates every 10 ms.
- **Expiry placement**: deadlines expire where the timer service ran in the
  step (after the prunes and the futex expiry), on every CPU; CPU 0's loop
  pass is its step. Up to 8 batches of 32 keys per step.
- **The idle step's return value** no longer reports a pending poll without
  a timeout; only a non-empty heap counts as a pending deadline.

## Tests

- New `test_wait_hash.py` (`tests/host/wait_hash_tests.cpp`, ten profiles,
  `OK_HOSTED_LOCK_MODEL` and `OK_LOCK_DEBUG`): FIFO takes, colliding keys,
  batches of 16, owner release along its chain, pool exhaustion; scheduler
  links, A2 wakes, owner unlink after a timeout and a cancel, exhaustion with
  its retry deadline, teardown; host-thread races: the A3 insert-then-check
  of a poll against a waker holding the object lock (20000 rounds, both
  interleavings, no lost wake), and wakes against owner unlinks (the pool
  ends empty).
- New `test_deadline_heap.py` (`tests/host/deadline_heap_tests.cpp`, ten
  profiles): 200000 random operations against a reference model, the full
  576-key capacity, one deadline per wait, removal on completion, the three
  actions, process keys, 40 waits in one expiry, lateness, and expiry racing
  completion on host threads.
- Adapted to the new contracts: `scheduler_wait_tests.cpp` (a completion
  removes the wait's heap key and leaves the sleep record to its owner; the
  capacity test wakes 512 waits through one channel, and would expire the
  retries of those beyond a smaller pool; hosted builds have 1024 entries),
  `native_terminal_signal_tests`
  (VTIME through the heap key), `native_console_control_tests` (the console
  wake completes the poll; the owner's step produces the result),
  `native_udp_receive_tests` and `tcp_immediate_runtime_tests` (the poll's
  entry on the socket channel replaces `socket_only`; an idle pass may report
  idle).

## Validation

See the result notes under `smp-scaling/waveB/WP4b` for the logs.
