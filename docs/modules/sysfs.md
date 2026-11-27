# CPU sysfs and user CPU hotplug

Userland takes secondary CPUs offline and back with Linux's interface: root
writes `0` or `1` to `/sys/devices/system/cpu/cpuN/online`. Threads pin
themselves with `sched_setaffinity`, read their mask with
`sched_getaffinity` and their CPU with `getcpu`. BusyBox `nproc` and musl
`sysconf(_SC_NPROCESSORS_ONLN)` work from these. Results and limits:
`docs/qualification/CPU_HOTPLUG_USER.md`. The kernel mechanism underneath is
`stop_secondary_cpu`/`restart_secondary_cpu` (`docs/modules/smp.md`, "CPU
hotplug").

## The provider

`fs::CpuSysFileSystem` (`ok/fs/sysfs.hpp`) is a fixed tree of nodes that live
as long as the object. It is constant-initialized, like the device filesystem.
Where it is mounted is distribution policy: ObfuscationOS mounts it at `/sys`
after `/dev`, which uses one of the eight VFS mount slots.

| Path below the mount | Mode | Content |
|---|---|---|
| `devices/system/cpu/kernel_max` | 0444 | `15` (`smp::max_cpus` - 1) |
| `possible`, `present` | 0444 | cpulist of the CPUs bring-up started |
| `online` | 0444 | cpulist of the CPUs online now |
| `offline` | 0444 | present and not online; an empty line when none |
| `cpuN/` | 0555 | every present N, `cpu0/` included |
| `cpuN/online` | 0644, root | N >= 1 only: `1` or `0` |

Directories above `cpu/` are 0755. Every node is owned by root, has inode
numbers fixed per node and fixed epoch timestamps. Attributes report a size of
4096, as Linux does.

- A CPU whose start failed is not present. `nr_cpu_ids` is the highest
  present CPU plus one.
- A CPU is online for userland when its state is `online`, it is in the
  `smp` online mask and the scheduler's online set has it
  (`smp::cpu_online_now`). A stop that timed out leaves the CPU `stopping`
  with the scheduler bit cleared, so its file reads `0`.
- A cpulist is Linux's format (`0-3,5`). `fs::cpulist` is constexpr; the
  longest list, sixteen CPUs alternating, is 19 bytes.
- The provider reads CPU state through a function pointer (`smp::cpu_view` in
  the kernel). Hosted models pass their own.
- UP profiles show `0` for `possible`, `present` and `online`, `cpu0/`
  without `online`, and nothing to write.

`concurrent_io()` is false, so kernel sections never serve these files.
`truncate` succeeds without effect (a shell's `>` truncates first). `create`,
`create_child` and `create_symlink` on a directory return `denied`; `chmod` and
`chown` return `permission`.

### Open (kernfs semantics)

`Node::control_node()` returns a `fs::ControlNode` (`ok/fs/control_node.hpp`),
next to `terminal_node()`. `PosixService::openat_limited` records it in the
open file description (`OpenFileDescription::control`):

- `attribute` (the 0444 files): opening for writing is EACCES, even for root.
- `cpu_online`, with the CPU number: ordinary permission checks, so only root
  opens it for writing.
- `cpu0/online` does not exist: ENOENT without `O_CREAT`, and EACCES with it
  because the directory refuses creation.

## The write route

`write`, `writev` and `pwrite64` check the description's control kind (a byte
compare) before every node path. A `cpu_online` description goes to
`cpu_control_write` or `cpu_control_writev` (`src/user/runtime/hotplug.cpp`).
The native personality reaches it from the step's dispatch. Linux `write` and
`writev` reach it through the shared native calls, and the Linux adapter
routes `pwrite64` before its positioned I/O. The offset of `pwrite64` is
ignored, as on Linux.

1. More than 4096 bytes is E2BIG; zero bytes returns 0.
2. Up to 16 bytes are copied and parsed with Linux `kstrtobool`: `1yYtT` and
   `on` are true, `0nNfF` and `of...` false; anything else is EINVAL.
3. The thread registers its wait first (`ThreadWaitReason::cpu_hotplug`, 16,
   key 0), then files the request (`smp::hotplug::submit`). The result is
   one of:
   - success with the byte count, when the CPU already is in the requested
     state (Linux returns success for a no-op);
   - EBUSY, when the `smp_hotplug_test` knob cycles that CPU, shutdown is
     pending, or the CPU is `stopping`, `starting` or `boot`;
   - EIO, when the CPU is `halted`;
   - armed: the request is filed in the writer's record.
4. A completed submission cancels the thread's own wait and returns.
5. An armed one sends CPU 0 a kick if the writer runs elsewhere and returns
   incomplete. The thread waits uninterruptibly (no signal ends the wait).

SIGKILL still ends a waiting writer: the kill sets the process's pending
exit and queues its parked threads; the first one picked exits the group,
and the retirement of each thread (`retire_native_thread`) drops its
record. Exec drops the record of every old thread too, the surviving
leader's included, since the new image keeps the leader's lease and wait
ticket. When the woken thread is dispatched again, the step's resume chain
finds the finished record (a lock-free hint per record) and returns the
stored result once, keyed by the current wait ticket, before anything decodes
the frame. The re-dispatch never reads the user buffer again, and a newly
trapped syscall never takes a stored result.

## Records and the service

`smp::hotplug::HotplugService<Backend>` (`ok/smp/hotplug.hpp`) holds one
record per entry-stack lease slot (`KernelEntryStackPool::capacity`, 128). A
native thread keeps its lease for its lifetime, so it owns at most one
request. A record is `{state, sequence, generation, cpu, want, pid, tid,
ticket, count, result}` under the `hotplug_request` lock (rank 88, a leaf).
States:

- `free`;
- `armed`: filed, its writer waits;
- `running`: taken by the service;
- `done`: a result its writer consumes once, keyed by its ticket;
- `abandoned`: the writer left while the service ran.

Records hold identities, never pointers. Teardown of a writer, or exec, frees
an armed or done record (counted `dropped_waiting` or `dropped_done`). A running one moves to a spare
record past the lease slots, marked abandoned, so the lease slot is free for
its next owner at once; the service frees the spare and counts it
`abandoned`. The service runs one request at a time, so one spare suffices. A
new request on a slot whose earlier result was never consumed replaces it.

`smp::hotplug_service_pass()` (`src/smp/hotplug.cpp`) runs on CPU 0 after
`ok_step_userland()` in the kernel loop. It runs outside the
`smp_hotplug_test` block, holding the kernel lock that `stop_secondary_cpu`
and `restart_secondary_cpu` require. One pass:

1. Recovers wedged CPUs (below).
2. CS1, under `hotplug_request`: takes the armed record with the lowest
   sequence and marks it running.
3. CS2: the writer must still wait on this ticket. Its thread must exist,
   be blocked with reason `cpu_hotplug`, and its registration must be
   pending. Otherwise the record is freed and counted `orphaned`.
4. Holding no record lock: checks the CPU's state again, then stops or
   restarts it. Both release the kernel lock while they wait.
5. CS3, under `hotplug_request`: stores the result if the record is still
   running with the same generation. An abandoned record (the spare) is
   freed instead.
   Then it completes the writer's wait, holding only the kernel lock.

CPU 0 is never a target: `hotplug_request` refuses CPU 0 and the caller's own
CPU. It holds no record while it serves and serves its interrupts while it
waits. The desktop loop does not idle while a request is pending
(`smp::hotplug::pending()`). Profiles that start no secondary compile the
pass to `return false`.

### Results

The service maps the CPU state first, then the `Status`:

| Outcome | Result |
|---|---|
| success, or the CPU already in the requested state | the byte count |
| `busy`: the hotplug turn is held, or `offline_cpu`'s precheck refused | EBUSY |
| `io_error`: the CPU did not park, did not come online or did not answer its IPI | EIO |
| `fault`: the CPU left with a user entry or section recorded (it is offline) | EIO |
| `invalid_argument` or `denied` (an inconsistent state after the recheck) | EIO |

Every EIO counts in `failures`. An online write that fails with EIO leaves
the CPU offline when its start failed (as Linux returns EIO for a CPU that
did not come up) and a later `1` tries again; only `ipi-timeout` leaves it
online.

### Failure reasons

`stop_secondary_cpu` and `restart_secondary_cpu` name why they failed
(`smp::transition_failure()`), and the service keeps each failure with that
reason until a report prints it (`OK_SMP_HOTPLUG_FAIL`, below):

| Reason | What failed |
|---|---|
| `halted` | the CPU is halted (a start gave up on it with another identity); refused before anything ran |
| `park-timeout` | a stop: the CPU did not park within 2 s (it is wedged) |
| `leave-fault` | a stop: the CPU left with a user entry or section recorded |
| `not-online`, `not-parked`, `request`, `scheduler` | an inconsistent state after the recheck |
| `tsc-absent` | an x86 restart: the CPU did not join the clock handshake within 2 s |
| `tsc-silent` | an x86 restart: the CPU stopped answering the handshake for 100 ms |
| `tsc-loose` | an x86 restart: no round of a measurement was within 50 us, so the offset is unknown |
| `tsc-refused`, `tsc-uncorrected` | an x86 restart: the CPU refused the offset, or a tight measurement still found it after the correction |
| `start-error` | a restart: the architecture's start request failed for another reason |
| `start-timeout` | a restart: the CPU did not come online within 2 s |
| `wrong-identity` | a restart: the CPU came up reporting another identity (it is halted) |
| `ipi-timeout` | a restart: the CPU came online but did not answer its IPI |

`tsc-absent`, `tsc-silent`, `tsc-loose` and `start-timeout` measure how
promptly the CPUs answer, not their clocks. Under QEMU TCG each vCPU is a
host thread; when the host is loaded they are rarely scheduled together, a
handshake round then spans host scheduling latencies, and a restart spends
its 16 batches of 64 rounds (9 to 18 s in loaded runs, holding the kernel
lock) without a round under 50 us. The x86 start fails closed: the CPU stays
parked and the write is EIO.

### Wedged CPUs

A stop that times out leaves the CPU `stopping`; topology marks it halted.
Its file reads `0` and writes to it are EBUSY. Each pass checks the wedged
CPUs. One that has parked since (state `offline` and `arch::smp::parked`) is
marked offline in the topology, and a later `1` restarts it. The report counts
`wedged`.

## Affinity

Each thread has its own mask (`ThreadControlBlock::cpu_affinity_mask`). The
process mask stays kernel placement policy: modules and drivers set it, and
exec resets it. The effective mask is process & thread. Fork and clone copy
the caller's thread mask. Exec gives the new image's only thread (the old
leader's TCB) the calling thread's mask, as Linux keeps the exec'ing task's.

Every placement honors the effective mask:

- `select_cpu`;
- `thread_admits` (the thread-aware `affinity_admits`);
- `eligible_on` (stealing);
- `dequeue_thread`;
- `mark_on_cpu`;
- `dispatch_selected`'s thread choice and `activate_thread`;
- the step's scan.

A thread none of whose allowed CPUs is online has its affinity broken
(`SchedulerState::repair_affinity`, Linux's `select_fallback_rq`): its thread
mask resets to every CPU and a record goes into a 16-entry ring. CPU 0
reports the ring at its next step. The repair runs wherever such a thread is
placed: in `select_cpu`, in the scan before it picks a thread directly, and
so in `offline_cpu`'s drain. `select_cpu`'s old fallback "return CPU 0" now
remains only for a process mask that names no configured CPU.

- `sched_getaffinity(tid, len, mask)`, tid 0 for the caller: EINVAL if
  `len * 8 < nr_cpu_ids` or `len` is not a multiple of the word size.
  Otherwise it writes effective & online as one native-endian word
  (big-endian on mips, mips64 and ppc) and returns the word size.
- `sched_setaffinity(tid, len, mask)`: reads up to one word (a shorter mask is
  zero-filled) and keeps only present CPUs. The caller needs euid 0, or an
  euid equal to the target's euid or uid (Linux `check_same_owner`); else
  EPERM. EINVAL if process & mask & online is empty; otherwise the mask is
  stored. A target running on a CPU outside its new mask is kicked.
- `getcpu(cpu, node, cache)`: the current CPU and node 0.
- An unknown tid is ESRCH; a bad pointer EFAULT.

The Linux numbers come from `scripts/generate_linux_abi.py` (musl 1.2.5).
The native personality uses the x86_64 numbers 203, 204 and 309 in
`syscall::Number`.

### offline_cpu and online_cpu

`SchedulerState::offline_cpu` first refuses with `busy` (EBUSY to the
writer) if a live process's process mask has no other online CPU. Then it
clears the CPU's bit and moves every entry queued there. A thread mask never
refuses: such a thread's affinity breaks when it is placed again.
`online_cpu` sets the bit again. Both run under the hotplug turn and the
kernel lock.

### The ticks

Once native userland has started (`SchedulerState::set_native_userland`),
`schedule_next_on_cpu`, which `Kernel::tick` and `PreemptionController::tick`
both call, places only `kernel_thread` records
(`schedule_kernel_thread_on_cpu`). Native threads belong to the step, which
honors their masks.

## Reports

With secondaries started, every interrupt report ends with

    OK_SMP_HOTPLUG_USER scope=hardware requests= offline= online= noop= failures= busy= orphaned= wedged= affinity_broken= max_us=

The fields:

- `requests`: every filed write;
- `offline` and `online`: successful stops and restarts;
- `noop`: writes that found the CPU already in the requested state, when
  filed or when served;
- `failures`: EIO results;
- `busy`: EBUSY results;
- `orphaned`: requests whose writer had gone;
- `wedged`: stops that timed out;
- `affinity_broken`: every broken affinity;
- `max_us`: the longest stop or restart.

Then every failure not reported yet, once, oldest first:

    OK_SMP_HOTPLUG_FAIL scope=hardware failure= cpu= want= errno= reason= us=

- `failure`: its number among the `failures`, from 1; the service keeps the
  latest 8 until a report takes them, so a gap means older ones were lost;
- `cpu` and `want`: the CPU and the value written (1 online, 0 offline);
- `errno`: what the writer got (5, EIO);
- `reason`: from the table under Failure reasons;
- `us`: how long the stop or restart ran (0 when refused).

It is followed by

    OK_SMP_HOTPLUG_RECORDS scope=hardware abandoned= dropped_waiting= dropped_done=

- `abandoned`: requests whose writer left (killed, or its image replaced by
  exec) while the service ran them; the CPU change still happened and nothing
  was completed;
- `dropped_waiting`: armed requests freed when their writer left while it
  waited for the service (nothing was attempted);
- `dropped_done`: results freed unconsumed when their writer left.

CPU 0 writes the service counters; writers write theirs under the record
lock. Each broken affinity also prints, from CPU 0 and never under a
scheduler lock,

    OK_SMP_AFFINITY_BROKEN pid= tid= cpu=

where `cpu` is the lowest CPU the thread was confined to.

## Latencies and limits

- No rate limit; root only, as on Linux.
- A stop can occupy CPU 0 for up to 2 s.
- An x86 restart holds the kernel lock across INIT and the TSC handshake;
  on a loaded TCG host that handshake can take 9 to 18 s and still fail
  (`tsc-loose`).
- Device lines are not moved back onto a restarted CPU.
- Everything here runs under the kernel lock (smp-scaling WP1): the route,
  the service, completion and wakeup.

## Tests

- `scripts/test_sysfs_cpu.py`: the tree, cpulists for UP, SMP, offline and
  wedged views, modes, strict lookup and the kernfs opens through the actual
  open path as root and uid 1000.
- `scripts/test_cpu_hotplug_service.py`: the service over a model backend:
  record lifecycle, consume-once results, no-op and coalesced requests,
  refusals, orphaned and abandoned records, a lease slot reused while its
  abandoned request runs, a result dropped by exec, the result mapping, the
  failure reports (reasons, numbering, a failed start left offline and
  restarted later, the ring dropping the oldest), a wedged CPU, service
  order and the parser. A race of host threads uses the real lock.
- `scripts/test_tsc_sync.py`: the x86 handshake's decisions name why an
  unsynced one failed (silent, loose, refused, uncorrected).
- `scripts/test_native_exec_eviction.py` (`exec_drops_leader_hotplug_request`):
  a sibling's exec while the leader waits in a hotplug write, through the
  actual runtime: the request is dropped and the new image keeps the caller's
  affinity.
- `scripts/test_run_queues.py` (`test_thread_affinity`, `test_kernel_tick`,
  `test_hotplug`): thread masks in every placement path, the stranded thread
  (queued, blocked or running on the CPU that leaves), the ring, the process
  mask precheck and the tick restriction.

All five are model scope. Guest evidence comes from ObfuscationOS:
`scripts/test_cpu_sysfs.py` and `scripts/test_smp_stress.py --user-hotplug`.
