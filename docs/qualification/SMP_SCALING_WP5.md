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
  (files -> ofd_table), dropped by the `PinnedDescription`. read, write,
  pread, pwrite, writev, lseek, fsync, ftruncate, fcntl and getdents keep the
  description pinned across their provider call.
- **Descriptor reservations** (spec 4.13): `reserve_descriptor_slot` marks the
  lowest free slot under `files`; `install_reserved_descriptor` publishes into
  it. openat, dup, memfd_create, the native UDP socket, pipe, socketpair and
  the AF_UNIX socket reserve before their work, so two threads opening at once
  get different slots.
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
- **Terminals** (`tty`, rank 36): the PTY table, the console terminal and its
  line discipline. A read is prepared and previewed (copied to a kernel
  staging buffer) in one tty hold, copied to the user without it, and
  committed under it against the preview's revision; a commit another reader
  or a writer overtook is busy and the read is prepared again
  (`read_pty_prepared`; the console read loops the same way). The eighth
  attempt dequeues while it stages, so a read always progresses; only its
  copy-out then follows the dequeue. PTY reads and writes, readiness and the
  console's input path hold the lock; wakes follow it.
- **Datagram transmit** (`net_stack`, rank 38, in `Kernel`): a UDP send offers
  its frame to the NIC at once (`Kernel::transmit_datagram`); a frame the NIC
  cannot take waits in a four-frame `TransmitBacklog` that the network poller
  offers again, oldest first; a full backlog refuses the frame and the send
  returns ENOBUFS (Linux and native sendto alike). The native sendto no longer
  waits for the transmit completion.
- **Per-CPU UDP scratch**: the Linux UDP operation and owned-receive scratch
  buffers are `smp::PerCpu` on x86_64, aarch64 and rv64; their `busy_` guards
  stay as same-CPU assertions.
- **VFS namespace** (`vfs_namespace`, rank 14, a `SharedGate` replacing
  `operation_active_`): namespace operations hold it shared, mount, detach,
  the archive import and `create_readonly_file` exclusive. Admission only
  tries the gate, so contention is still the deferred `busy` the callers
  queue on; the deferred waits become ready when the last holder leaves.
- **RAM filesystems** (`ram_topology`, rank 39): entry changes (attach,
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
  line's handlers is served at once). `unbind` removes the handler under the
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
  pipe no-lost-wake (`arm_pipe_wait` vs a writer); unix GC vs send; tty
  commit retry (two readers and a writer on one PTY, plus a deterministic
  stale commit); RAM rename vs lookup; two appenders. Every two-sided race
  forces each order in some rounds and races freely in the others.
- `test_irq_wait_time.py` gains unbind vs a running drain (200 rounds; the
  context is freed after unbind returns and the handler must not touch it),
  the pending recheck of an edge during the handlers against a concurrent
  drain, and a handler unbinding its own line.
- `test_ext4_gate_concurrency.py` is generalised to the provider gates: the
  ext4 tests now inspect the `ServingGate`, and a new simplefs test
  (`tests/host/provider_gate_tests.cpp`) checks concurrent I/O, same-CPU
  busy, a SPINNING waiter and the `fs_provider` rank.
- New `test_transmit_backlog.py`: order, ENOBUFS, drops and wrap-around.
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

## Validation

All guest runs are QEMU 10.2 TCG from the SysToolchain (`scope=hardware`
means QEMU device execution); model tests are `scope=model`. Kernel sources
of every run below equal f601e29's; the OS is b8dceae plus this package's
OS patch. Logs: `~/.cache/smp-scaling/waveB/WP5/logs2/`.

- **Hosted suite** (`--jobs 8`): 173 passed, 2 failed, 59 not run. The two
  failures are the WP4 race runners' interleaving-coverage checks under the
  suite's load (`wait_park_race_tests.cpp:178`, `wait_hash_tests.cpp:444`;
  no lost wake). Both pass alone; under four concurrent runners
  `wait_park_race` fails the same check on the start commit d7fde31 too, and
  `wait_hash` (code WP5 does not touch) failed once on f601e29 and not on
  d7fde31 in two rounds. New runners `test_descriptor_races.py` and
  `test_transmit_backlog.py` pass, as do the adapted ones.
- `test_net_runtime.py --sanitize`: 18/18 socket suites pass.
- `xmake test`: `-smp 1` on the ten profiles and `-smp 2`/`4` on x86_64,
  aarch64 and rv64, 16/16 `OK_TEST_PASS`.
- **distro10** (native distribution, `--require-native`): ten profiles
  PASS, every `OK_SMP_KERNEL_CHECKS` counter 0, `unqueued=0`.
- **Gates** on x86_64, aarch64 and rv64: `--user-hotplug` debug at 2 and 4
  vCPUs, release at 2 and 4, `PROBE=futex_smp_atomic` at 2,
  `--smp_hotplug_test=y --require-hotplug` at 2, `SECTIONS=off` at 2; the CPU
  sysfs probe at 4 on x86_64 and at 1 on arm32. All pass with every check
  counter 0, `unqueued=0`, `max_lock_depth` 5 (debug) and 0 (release). Every
  stress run counts metadata, pipes and ptys = 240 and `udp_nolink` = 240.
  Two x86_64 first runs failed on an x86 restart under host load and passed
  when rerun: uh2 (`chcpu -e` got EIO, the user-hotplug request counted
  `failures=1`) and hot2 (`OK_SMP_HOTPLUG_FAILURE phase=restart reason="x86
  secondary TSC was not synchronized"`, the same failure WP4d's hot2 logged).
  A second hot2 attempt failed at boot with `OK_TSC_SYNC result=unsynced`
  (secondary start failed before userland); the third passed. No run showed
  a kernel failure line or a nonzero check counter.
- `OK_SMP_LOCKS` at 4 vCPUs (debug): no class A contention (everything still
  nests in the BKL); `fs_provider` 600 to 800 contended of about 81,000
  (sections run ext4 file I/O without the BKL).
- **OS runners** (SDK-only and distribution): `test_virtio_block.py`
  (legacy and modern) on x86_64, aarch64 and rv64; `test_virtio_net.py`
  (x86_64), `test_virtio_net_mmio.py` (aarch64, rv64); `test_static_udp.py`
  and `test_virtio_poll.py` on the three; on x86_64
  `test_native_udp_socket.py` (native sendto through the backlog to an
  external peer), `test_persistent_ext4.py --require-native` and
  `test_unix_rights.py` (two guest runs). All pass.
- Not run: `test_unix_pathname.py` accepts only an ABI 65 SDK
  (`--expected-abi` must be 65); there is no current consumer to run it.
