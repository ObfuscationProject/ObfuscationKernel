# SMP scaling WP5: files, VFS, IPC, tty, net and devices

Lock-placement package of the smp-scaling work (spec sections 3.6, 3.7,
4.13). The descriptor table, the open file description table, pipes, AF_UNIX
rights, terminals, the datagram transmit path, the VFS namespace, the RAM
filesystems, the filesystem providers and the interrupt router get the locks
the later packages need to run them without the big kernel lock (BKL). Every
path still runs under the BKL: WP7a flips the families. The fine locks nest
inside it and are order checked in debug kernels. Baseline: d7fde31 (WP4b
after its review). The module ABI is not bumped; exported layouts change
(`ProcessResources`, `PipeBuffer`, `SchedulerState`, `Kernel`, the `Node`
vtable, `VirtualFileSystem`, the ext4, FAT and simplefs providers,
`IrqRouter`), so every OS object must be rebuilt against this SDK.

## What it adds

- **Descriptor table** (`files`, rank 30, one per process in
  `ProcessResources::files_lock`, a spin lock on every profile because the
  record sits in the exported layout). Slot writes (install, close,
  reservations, publication of prepared operations, rights receives, fork's
  copy) happen under it; no provider call, release or user copy does.
- **Open file descriptions** (`ofd_table`, rank 32, one per scheduler):
  allocation, references, AF_UNIX endpoints and rights queue edges. The
  release is split (spec 3.6): the reference, the slot and the pipe endpoint
  counts change under `ofd_table`; the pipe and PTY wakes follow; the provider,
  socket path, shared-file, TCP and UDP releases run with no scheduler lock.
  A description's first reference retains its node outside the lock (a
  provider's retain may enter its gate). Only a last reference wakes the
  pipes' peers, so a pin or a duplicate going wakes nobody.
- **RetireList**: `close_descriptors` clears up to 32 slots per `files` hold
  and releases them after dropping it, continuing from its cursor;
  `close_range` and exec's close-on-exec pass use it.
- **pin(fd)** (`SchedulerState::pin_descriptor`, `PosixService::pin`): the
  slot is read under `files`; a caller whose process has more than one live
  thread (or an unknown caller) also takes a reference under `ofd_table`
  (files -> ofd_table), dropped by the `PinnedDescription` (a last release
  it runs records its failure like a close). read, write, pread, pwrite,
  readv, writev, lseek, fsync, ftruncate, fcntl, getdents, ioctl, fstat,
  fchdir, fchmod, fchown, futimens and fstatfs on an open file, faccessat
  with AT_EMPTY_PATH, dup, dup2 and F_DUPFD keep the description pinned
  across their provider call or publication.
- **Path walk start** (spec 3.7): `path_context` returns a
  `HeldResolveContext` whose start node is retained for the walk. A dirfd's
  node is retained through its pin. The cwd is read under `files`, retained
  outside it and read again: a chdir in between gives the stale reference
  back and the read repeats (at most 8 times, then busy). chdir and fchdir
  swap the cwd under `files` and release the old one after it; umask and
  F_SETFD change their fields under `files` too. An AT_EMPTY_PATH call on
  the cwd (fstatat, fchmodat, fchownat, faccessat) uses the same retained
  start.
- **Descriptor reservations** (spec 4.13): `reserve_descriptor_slot` marks the
  lowest free slot under `files`; `install_reserved_descriptor` publishes into
  it. openat, dup, memfd_create, the native UDP socket, the TCP socket and
  accept (lowest slot from 3, as before), pipe, socketpair and the AF_UNIX
  socket reserve before their work, so two threads opening at once get
  different slots. The TCP paths allocate their description under
  `ofd_table` once the stream exists and give the slot back on failure.
- **Pipes** (`pipe`, rank 34, one per buffer): ring, revision, rights records
  and the new `readers`/`writers`/`rights_count` counts, replacing the scan of
  every description. A description counts from its first reference to its
  last release (allocation, reservation publication and AF_UNIX connect
  included). Debug kernels compare every count with the old scan and count a
  difference in `audit_repairs` (the scan wins). Buffers are claimed and
  retired under `ofd_table` -> `pipe`. Reads consume under the pipe lock and
  release a completed rights record after it. `arm_pipe_wait` registers a
  runtime pipe read or write wait under the pipe lock and completes it at
  once when the condition the caller saw is gone (spec 4.3 A1).
- **AF_UNIX rights** under `ofd_table` -> `pipe`: send takes the rights'
  references and queue edges and fills the record in one hold; receive
  previews and reserves under `files` -> `ofd_table` -> `pipe`; the
  collection runs only while some pipe holds a rights record, builds its
  graph under `ofd_table` (one pipe lock at a time) and releases what it
  detached after dropping it. The "collecting" flag is claimed under
  `ofd_table`.
- **Terminals** (`tty`, rank 36): the PTY table, every terminal, the console
  lease and `/dev/pts`. Under it: PTY reads, writes, readiness, pair
  creation, binding, the final retire and `/dev/pts` removal, the PTY lock,
  index and pts mode/owner changes; every terminal ioctl but TIOCSCTTY and
  TIOCSPTLCK, which lock inside (termios, flushes, window, foreground,
  queue sizes); the acquisition and the hangups; the console lease's claim,
  revoke, flush, admission and retirement; the console's echo, output
  processing and outbox (an outbox entry is taken under it and its signal
  sent after it). Wakes take only higher ranks, so a few run under it.
  A read is prepared and previewed (copied to a kernel staging buffer) in
  one tty hold, copied to the user without it, and committed under it
  against the preview's revision; a commit another reader, a writer or new
  input overtook is busy and the read is prepared again
  (`read_pty_prepared`, and the console read). The eighth attempt stages and
  dequeues in one hold (`stage_pty_read`, `stage_console_read`), so a read
  always progresses; only its copy-out follows the dequeue.
- **Datagram transmit** (`net_stack`, rank 38, in `Kernel`): a UDP send offers
  its frame to the NIC at once (`Kernel::transmit_datagram`); a frame the NIC
  cannot take waits in a four-frame `TransmitBacklog` that the network poller
  offers again, oldest first; a full backlog refuses the frame and the send
  returns ENOBUFS (Linux and native sendto alike; see the deviations). The
  native sendto no longer waits for the transmit completion. The runtime
  report prints `OK_NET_TX scope=hardware direct= queued= flushed= refused=
  dropped= pending=` once a datagram was offered.
- **Per-CPU UDP scratch**: the Linux UDP operation and owned-receive scratch
  buffers are `smp::PerCpu` on x86_64, aarch64 and rv64; their `busy_` guards
  stay as same-CPU assertions.
- **VFS namespace** (`vfs_namespace`, rank 14, a `SharedGate` replacing
  `operation_active_`): namespace operations hold it shared, mount, detach,
  the archive import and `create_readonly_file` exclusive. Admission only
  tries the gate, so contention is still the deferred `busy` the callers
  queue on; the deferred waits become ready when the last holder leaves.
- **RAM filesystems** (`ram_topology`, rank 39, one per VFS for all its RAM
  trees; see the deviations): entry changes (attach,
  detach, link, rename) and every lookup step that retains or exposes a child
  hold it before any node lock, so a rename is atomic against lookups and the
  `parent_` walk of `directory_path`. The node and file-buffer pools are under
  the unranked leaf `ram_pool_`.
- **Provider gates** (`fs_provider`, rank 18, `smp::ServingGate`): the ext4
  operation gate is re-based on it (identity checks after the claim; a
  refused claim on a mounted filesystem drains the detached nodes a release
  may have left it); FAT's busy flag and simplefs's spin lock (held across
  device I/O) become the gate.
- **Atomic O_APPEND** (`Node::append`): reads the end and writes there in one
  step: under the node lock for RAM, in one gate operation for ext4; the
  default (size, then write) is atomic only under the BKL. write() with
  O_APPEND uses it.
- **Interrupt router**: a line's handlers run on one CPU at a time
  (`running`, the draining CPU, set under `irq_router`); a drain that finds a
  line running gives its pending flag back and the running CPU serves it at
  its recheck after clearing `running` (an edge during an unmasked I/O APIC
  line's handlers is served at once). A drain runs a line's handlers at most
  twice, the run and one recheck: a line that is pending again after that (a
  device that keeps edging) keeps its flag, with the any-pending flag set,
  for the next drain, so one line cannot hold a drain. `unbind` removes the handler under the
  lock and waits, outside it and serving interrupts, until no other CPU runs
  the line's handlers, so the caller may free the context; a handler that
  unbinds its own line does not wait for itself.
- **Device waits**: `waiting_line` per CPU (`wait_for_line`): a top half IPIs
  only CPUs waiting for its line or for any line; a device waiter does not
  halt while another CPU runs a bottom half (its completion ends no halt).
- **OS (patch)**: virtio-blk and virtio-rng requests run under a per-device
  `ServingGate<block_request>`; the virtio-net rings under a
  `RankedSpinLock<device>` held for memory and register work only (never
  across the transmit wait); `DeviceSignal::take` is one word exchange on
  the three SMP profiles; the drivers wait on their own bound line. The
  stress probe gains RAM metadata (create, lookup, rename, O_APPEND, unlink),
  pipe ping-pong, PTY echo and datagram socket rounds, which the runner
  requires.

## Deviations from the specification

- **The cwd is retained outside `files`.** Spec 3.7 retains the cwd under
  `files`, which needs every provider's `Node::retain` to be a leaf (spec
  3.6). FAT's retain enters its provider gate (class B), which may not be
  taken under a class A lock. The cwd is therefore read under `files`,
  retained after it and read again; a stale retain is given back. Provider
  nodes stay in their filesystem's pool while it is mounted, and a mount
  that holds a cwd cannot be detached, so the retain never touches freed
  memory. Making FAT's reference count a leaf would allow the spec's form.
- **One `ram_topology` per VFS, not per RAM filesystem.** Every RAM tree of a
  VFS (its RAM mounts and owned RAM mounts) draws its nodes from the VFS's
  one node pool (`nodes_`), and a node does not record its tree. A lock per
  tree needs each node tagged with its tree's lock; until then lookups in
  one RAM tree serialize against entry changes in another (correct, less
  parallel).
- **The last read attempt loses its bytes on a fault.** The eighth attempt of
  a PTY or console read dequeues before its copy-out; an EFAULT there loses
  the dequeued bytes, as Linux's `tty_read` does through its kernel buffer
  (since 5.11). It is reached only after seven overtaken commits whose
  copies to the same buffer succeeded. Earlier attempts consume nothing on
  a fault.
- **ENOBUFS is not Linux's datagram behaviour.** Linux `udp_send_skb` turns a
  full device queue into success (counting `SNDBUFERRORS`) unless
  `IP_RECVERR` is set. Here a full four-frame backlog returns ENOBUFS from
  both the Linux and the native sendto (spec 4.13); before WP5 the Linux
  sendto failed at once when the NIC would block and the native sendto
  waited for the transmit.
- **Terminal checks read without `tty`.** `terminal_check`,
  `controlling_terminal` and the job-control facts read terminal fields
  without the lock (TIOCSPGRP iterates processes under it). They are reads
  of single words under the BKL; WP7a takes the lock for them or makes the
  fields atomic.

- **Leaf-level locks, not long holds.** Under the BKL, nested scheduler calls
  are common (a release releases rights records, which release
  descriptions). The `files`, `ofd_table` and `pipe` locks are held around
  memory work at the leaves, never across a call into another locking
  function, so no same-rank recursion arises. The spec's per-operation
  critical sections (for example open's "ofd_table allocate, then files
  publish") are kept as order, not as one hold.
- **`rights_in_flight`** is summed from per-pipe `rights_count`s (16 loads)
  instead of one counter under `ofd_table`: a record is consumed under its
  pipe lock alone, where `ofd_table` (a lower rank) cannot be taken.
- **The UDP and TCP services stay under the BKL.** `net_stack` guards the
  transmit backlog and the try-transmit; the services, `TcpStreamWaits` and
  the Ethernet executor's poll are not under it, because the TCP waits
  release descriptions (`ofd_table`, a lower rank) from inside the poll. WP7a
  has to split that release out before the poll can hold `net_stack`.
- **No journalled ext4 per-mount gate.** The journalled mount
  (`Ext4MountedMetadata`, `Ext4OwnedTransaction`, include/ok/fs/ext4/) is
  instantiated only by host and native tests; no kernel source under `src/`
  mounts it, so nothing reaches it outside the provider gate of the mounted
  ext4. It keeps its admission flags under its unranked `lock_` (busy on
  contention). The gate belongs to the change that wires it into a mount.
- **`drain_platform_irqs` keeps `require_exclusive`.** Bottom halves leave the
  BKL in WP7a; the running flag makes concurrent drains safe already.
- **No loopback datagrams in the stress probe.** The native UDP service has
  no loopback route and the stress boot has no NIC, so the datagram round
  exercises socket admission, ephemeral bind, sends to the static link peer
  and close; a refused socket counts `udp_nolink`, and udp + udp_nolink must
  equal the rounds.
- **No stat in the metadata round.** The musl probe's stat returns
  EOPNOTSUPP on the scratch ext4 and on /run alike in this boot, so the
  round (under /run, the RAM filesystem the new locks cover) looks names up
  with open and reads sizes as end-of-file offsets.
- **pwrite with O_APPEND** still reads the size and writes (under the
  position lock and the BKL); its offset limit check comes before the write.
- **The runtime's begin/resume pipe paths** keep their non-blocking attempt,
  then arm with `arm_pipe_wait`, instead of one combined locked call.

## Tests

- New `test_descriptor_races.py` (`tests/host/descriptor_race_tests.cpp`, ten
  profiles, `OK_HOSTED_LOCK_MODEL` and `OK_LOCK_DEBUG`, host threads on the
  real locks): close vs pinned read; two-thread openat (slot reservation);
  pipe no-lost-wake (`arm_pipe_wait` vs a writer); unix GC vs send (a
  right stays in flight on a seed pair for the whole run, so every release
  runs the collection, and a third CPU runs it against the send); tty
  commit retry (two readers and a writer on one PTY, plus a deterministic
  stale commit); RAM rename vs lookup; two appenders. Every two-sided race
  forces each order in some rounds and races freely in the others.
- `test_irq_wait_time.py` gains unbind vs a running drain (200 rounds; the
  context is freed after unbind returns and the handler must not touch it),
  the pending recheck of an edge during the handlers against a concurrent
  drain, a handler unbinding its own line, and a flooding edge line (its
  handler re-pends it on every run): a drain serves it twice and returns
  with the line still pending.
- `test_ext4_gate_concurrency.py` is generalised to the provider gates: the
  ext4 tests now inspect the `ServingGate`, and a new simplefs test
  (`tests/host/provider_gate_tests.cpp`) checks concurrent I/O, same-CPU
  busy, a SPINNING waiter and the `fs_provider` rank.
- New `test_transmit_backlog.py`: order, ENOBUFS, drops, wrap-around and the
  `OK_NET_TX` counters (direct, flushed).
- Adapted: `linux_udp_runtime_dispatch_tests` (a would-block device queues
  the frame; a full backlog is ENOBUFS; a link that refuses drops it),
  `linux_udp_receive_ownership_tests` (per-CPU scratch accessor), the VFS and
  ext4 admission tests (`operation_active()`, the gate's owner).
- Behaviour change in two existing tests: a pipe wait holds its description,
  so `dup2` over the waited slot drops a reference that is not the last and
  wakes nobody (before, a spurious wake). `native_pipe_read_continuation`
  now expects the wait to stay pending until A's data arrives;
  `native_syscall_origin` checks that the wait stays pending and forces the
  spurious completion itself, so its rearm checks still run.
- `unix_rights_tests` (in `test_native_pty.py`) caught the send's and the
  receive's reference-limit checks counting a right passed twice as one;
  both now count each description's occurrences before any change.

## Review fixes

An adversarial review of 697c2c5 raised twelve findings. Fixed: the missing
pins and the raw path walk start (spec 3.6, 3.7); TCP socket and accept
picking a slot without a reservation; the tty lock missing from the PTY
table, termios, flushes, `/dev/pts`, the console lease, echo, output and the
outbox; the console read failing the runtime after eight overtaken commits
(it now has the PTY read's bounded last attempt); a node reference leaked
when a first retain lost its description; a pin's last release dropping its
status; an IRQ drain staying on a flooding edge line; the unix GC vs send
model never running the collection against a send. Also: chdir, fchdir,
umask and F_SETFD write under `files`; the debug pipe-count audit runs only
where the description table cannot move; the transmit backlog is offered
again right after the poll pass reaped the ring; `OK_NET_TX` reports how
datagrams reached the NIC. Recorded as deviations instead: one
`ram_topology` per VFS, the last read attempt's fault loss, ENOBUFS versus
Linux. Still open: the SMP stress boot has no NIC, so its datagram round
counts only `udp_nolink`.

## Validation

All guest runs are QEMU 10.2 TCG from the SysToolchain (`scope=hardware`
means QEMU device execution); model tests are `scope=model`. Kernel sources
of every run below equal 87e8b53's; the OS is b8dceae plus this package's
OS patch. Logs: `~/.cache/smp-scaling/waveB/WP5/logs3/` (the runs of the
first continuation, at f601e29, are in `logs2/`).

- **Hosted suite** (`--jobs 8`, sources at 8453cc1, which differ from
  87e8b53 only in `Kernel::poll_network`): 173 passed, 2 failed, 59 not
  run. The failures are the WP4 race runners' interleaving-coverage checks
  under the suite's load (`wait_park_race_tests.cpp:178`,
  `wait_hash_tests.cpp:444`; no lost wake), as in the first continuation.
  Rerun alone at 87e8b53 `test_wait_hash.py` passes; `test_wait_park_race.py`
  failed the same coverage check in two of three runs while the gates ran,
  and the start commit d7fde31 passed two runs beside it (in the first
  continuation it failed two of two). `test_shutdown_cross.py`, the one
  suite runner that compiles `kernel.cpp`, passes at 87e8b53.
  `test_descriptor_races.py` (with the new GC model), `test_irq_wait_time.py`
  (with the flood test) and `test_transmit_backlog.py` pass.
- `test_net_runtime.py --sanitize`: 18/18 socket suites pass (the TCP socket
  and accept reservation).
- `xmake test`: `-smp 1` on the ten profiles and `-smp 2`/`4` on x86_64,
  aarch64 and rv64, 16/16 `OK_TEST_PASS`.
- **distro10** (native distribution, `--require-native`): ten profiles
  PASS, every `OK_SMP_KERNEL_CHECKS` counter 0, `unqueued=0`.
- **Gates** on x86_64, aarch64 and rv64: `--user-hotplug` debug at 2 and 4
  vCPUs, release at 2, `PROBE=futex_smp_atomic` at 2,
  `--smp_hotplug_test=y --require-hotplug` at 2, `SECTIONS=off` at 2; the CPU
  sysfs probe at 4 on x86_64 and at 1 on arm32. All pass with every check
  counter 0, `unqueued=0`, `max_lock_depth` 5 (debug) and 0 (release); every
  stress run counts metadata, pipes and ptys = 240 and `udp_nolink` = 240.
  The first x86_64 hot2 failed only at the hotplug requirement: the stress
  passed, 6 of 1669 restarts failed with `x86 secondary TSC was not
  synchronized` under host load (the known x86 restart issue), every check
  counter 0; the rerun passed (1166 cycles, 0 failures).
- `OK_SMP_LOCKS` at 4 vCPUs (x86_64 debug): no class A contention;
  `fs_provider` 879 contended of 81,564 (sections run ext4 file I/O without
  the BKL); `tty` now counts the console's echo and output too.
- **OS runners**: `test_virtio_block.py` (legacy and modern),
  `test_virtio_net.py` (x86_64) or `test_virtio_net_mmio.py` (aarch64, rv64),
  `test_static_udp.py` and `test_virtio_poll.py` on x86_64, aarch64 and rv64;
  on x86_64 `test_native_udp_socket.py` at 1 and at 2 vCPUs (the 2-vCPU run
  requires `OK_NET_TX`: direct=1 queued=0 refused=0),
  `test_persistent_ext4.py --require-native` and `test_unix_rights.py`. All
  pass.
- Not run: `test_unix_pathname.py` accepts only an ABI 65 SDK; the OS virtio,
  UDP-socket and unix-rights runners on the seven UP profiles; release
  gates at 4 vCPUs in this continuation (they passed at f601e29).
