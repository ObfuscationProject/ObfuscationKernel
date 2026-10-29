# ObfuscationKernel implementation plan

Status: active plan, written 2026-10-24 from the architecture review. The
long-term target and milestone gates stay in the ObfuscationOS
[workstation plan](../../ObfuscationOS/docs/plans/README.md); this document is
the kernel-side execution plan, organised as worktrees. The workspace-level
sequence and dependency graph are in the umbrella repository `PLAN.md`.

## Rules

1. One worktree per topic under `../wt/kernel-<topic>`, branch `feat/<topic>`,
   `refactor/<topic>` or `chore/<topic>`. Lifetime at most 4–6 weeks; merge
   before opening a successor.
2. A worktree that changes exported headers, the UAPI or the SDK manifest bumps
   `kernel_module_abi_version` once, at merge, and has a paired ObfuscationOS
   worktree that consumes the new SDK through `--kernel_sdk`.
3. Fast lane for every worktree: x86_64, arm32 (32-bit) and ppc (big-endian)
   with `xmake -b okernel && xmake test`. Before merge: all ten profiles plus
   `kernel-sdk` export and the paired OS consumer.
4. Receipts go to ObfuscationEvidence under `receipts/ObfuscationKernel@<sha>/<branch>/`.
   Per-slice notes go to `docs/qualification/`; subsystem documents under
   `docs/modules/` describe current behavior.
5. Tests move with code. Model tests keep their `scope=model` label; QEMU
   execution is `scope=hardware`; neither claims physical hardware.
6. Freestanding C++23, static dispatch for architecture/scheduler/hot paths,
   runtime virtual interfaces only at device/filesystem/module boundaries, the
   allocation contract in `DEVELOPMENT.md`.

## P0 (done on `chore/land-2026-10`)

- Landed five weeks of uncommitted work as ten subsystem commits; tag
  `sdk-abi74-uapi0.16`.
- CI resolves SysToolchain from its release archives (`build_from_source` is
  the fallback), caches the installed package per architecture, installs the
  QEMU runtime libraries, times jobs out after 90 minutes and exports the SDK
  artifact after `xmake test`.
- Arm kernel code is compiled with `-mstrict-align` (aarch64) and
  `-mno-unaligned-access` (arm32). It runs with the MMU off, where every data
  access is Device memory and an unaligned access faults; QEMU 8.2 tolerated
  that, the pinned QEMU 10.2.0 does not, and both profiles stopped at boot
  until the change. All ten profiles pass `xmake test` on the packaged QEMU.
- 87 per-slice notes moved to `docs/qualification/` with links rewritten; module
  ABI history moved to the end of `docs/modules/modules.md`; ten-profile userland
  statement fixed in `ARCHITECTURE.md`.

## P1: foundations

### WT1.1 `feat/mm-general` (critical path, XL, ABI → 75)

Scope
- Real memory map into `kernel_main` on all ten profiles: E820 (x86), FDT
  `/memory` and `/reserved-memory` (aarch64, arm32, rv64, rv32, loongarch64),
  board tables (Malta, ppce500). Reserve kernel image, boot stacks, page tables,
  firmware and device windows explicitly. Remove the synthetic 64 MiB fallback
  from product boots; keep it for hosted model tests only.
- `FrameAllocator` with runtime-sized metadata (bitmap or buddy over the
  discovered ranges), replacing the fixed 16,384-page table. Keep the
  owner/generation handle contract.
- Kernel object allocator (slab/size classes) under the allocation contract:
  fallible, accounted, with allocation contexts and per-owner charges.
- Process memory: per-process page charges and quotas replace the 16 address
  spaces / 128 pages / 4 MiB pool limits; copy-on-write fork with eager fallback
  kept behind a build option until COW passes all ten profiles.
- Page cache skeleton: page-sized file pages owned by the VFS node, used by
  shared file mappings in WT1.5/WT3.1 and by storage write-back in WT2.4.

Exit gate
- Ten native guests pass allocate / map / protect / pressure / fork tests
  without corrupting kernel or peer processes; the `posix-check` pressure phase
  runs against real quotas.
- No remaining fixed user pool constants in `include/ok/user` and `include/ok/memory`.

Paired OS worktree: `chore/sdk75-consumer`. Depends on: P0.

Status on `feat/mm-general` (ABI 75; paired OS worktree `chore/sdk75-consumer`):
- Boot memory on all ten profiles ([BOOT_MEMORY_MAP](qualification/BOOT_MEMORY_MAP.md)):
  E820, FDT, LoongArch UEFI system table → FDT, Malta PROM-sized board table;
  explicit image, boot stack/page-table, firmware, loader-table, page-zero and
  device-window reservations; `kernel_main` → `KernelConfig::physical_memory`;
  the synthetic 64 MiB map exists only in hosted builds.
- `FrameAllocator` metadata is runtime-sized (range table, bitmap, one
  `PageDescriptor` per frame) and carved from discovered RAM by `MemoryManager`;
  the owner/generation handle contract is kept. The `RuntimeFrameAllocator`
  scaffold folded into it.
- `SlabAllocator`: size classes 16..1024 plus contiguous large objects, page and
  per-owner byte charges, `boot`/`kernel`/`atomic` contexts with an atomic frame
  reserve, empty-page reclamation.
- The native page pool shares the kernel allocator (descriptor-held registry,
  object reserve, user quota); the 1024-frame pool and linker arenas are gone
  from kernel builds.
- Per-process charges: mapped user pages against each process's `RLIMIT_AS`,
  brk against `RLIMIT_DATA`, inherited by fork, kept by exec; native
  `getrlimit`/`prlimit64` ([PROCESS_MEMORY_QUOTAS](qualification/PROCESS_MEMORY_QUOTAS.md)).
- `fs::FilePageCache` skeleton owned by VFS nodes (`Node::page_cache()`).
- Copy-on-write fork is the default, with eager copies kept behind
  `--native_cow_fork=n` ([NATIVE_COW_FORK](qualification/NATIVE_COW_FORK.md)):
  fork shares private frames by reference with write withheld in parent and
  child records; user write faults and kernel `user_range` writes resolve by
  copying or, for the last owner, in place; every backend gains
  `execution::remap`.
- No fixed user memory pool remains
  ([NATIVE_TABLE_STORAGE](qualification/NATIVE_TABLE_STORAGE.md)): backends take
  translation tables from the page pool's object reserve and keep growable slot
  tables, a process's mapping records are contiguous runs bounded by the user
  window, shared memory files (SDK profile v3) keep their table, page indexes
  and ledger in object runs, and the software VM model moved to the kernel
  tests. The object reserve scales with discovered RAM (a sixteenth of the free
  frames, at least 256 pages, plus the runtime objects).
- Verification: `xmake test` on all ten profiles under the pinned QEMU 10.2.0
  (including a discovered-RAM frame/slab/page-cache test point); the paired OS
  distro boots natively on all ten with `qemu-distro --require-native`, whose
  `posix-check` pressure phase now runs against a lowered `RLIMIT_AS`
  (`OK_VM_QUOTA`), maps past the former 1024-record bound, and whose fork copy
  probe (`OK_VM_FORK_COPY`) passes in the default copy-on-write and the eager
  fallback builds alike, the former with `OK_NATIVE_COW` showing shared, copied
  and in-place resolved pages on every profile; hosted suites
  `test_boot_memory`, `test_physical_map`, `test_frame_allocator`, `test_slab`,
  `test_page_cache`, `test_native_page_pool`, `test_native_mapping_pages`,
  `test_mapping_view`, `test_native_vm`, `test_process_resources`.
Carried forward (outside the exit gate):
- No reclaim or OOM policy beyond fallible failure; IRQ/SMP synchronization of
  the allocators arrives with WT1.2 and WT2.1.
- `sched::max_processes` (64) bounds native processes; the remaining constants
  in `include/ok/user` are subsystem object capacities, not memory pools, and
  `include/ok/memory` keeps only the firmware input bound
  (`max_discovered_regions`).
- Shared memory files keep eager frames of their own; moving them onto
  `fs::FilePageCache` belongs with shared file mappings (WT1.5/WT3.1).

### WT1.2 `feat/irq-wait-time` (L, ABI)

Scope
- Interrupt controller drivers per profile: 8259/APIC, GICv2, PLIC, LoongArch
  EIOINTC, MIPS CP0/i8259 (Malta), MPIC (ppce500). Shared IRQ registration API
  in `include/ok/interrupt`.
- Virtio blk / net / input completion through interrupts; polling kept only as
  an explicit fallback.
- Wait queues connected to interrupt wakeups: `wait_registration` gains an IRQ
  source; bounded top halves, deferred work queue for parsing and callbacks.
- Calibrated monotonic and realtime clocks that advance while idle; POSIX
  timers and `setitimer`.

Exit gate: no polling loop remains in the kernel block/net/input paths on ten
profiles; clock drift test against QEMU wall time; timer interrupt latency
measured. Depends on WT1.1 (allocator for work items).

Scaffold (merged to main from `feat/irq-wait-time`) (hosted tests pass):
- `include/ok/interrupt/irq_chip.hpp`: `IrqChip` concept (initialize, configure,
  mask, unmask, acknowledge, end_of_interrupt), `ModelIrqChip`, `IrqRouter<Chip>`
  binding lines to dispatcher vectors; `InterruptDispatcher::dispatch_vector`.
- `include/ok/sched/wait_queue.hpp`: `WaitQueue<N>` over `WaitRegistration` with
  prepare / recheck / cancel / wake_one / wake_all and wake-source counters.
- `include/ok/time/clocksource.hpp`: `ClockSource` concept, `ModelClockSource`,
  `CalibratedClock<Source>` (nanoseconds without 128-bit arithmetic, deadlines).
- `tests/host/irq_wait_time_tests.cpp`, `scripts/test_irq_wait_time.py`.
Status on `feat/irq-wait-time` (opened 2026-10-29, Kernel issue #4; paired OS
worktree `feat/irq-wait-time`). Monotonic clocks are already counter-backed on
all ten profiles ([PLATFORM_CLOCKS](qualification/PLATFORM_CLOCKS.md)); the
kernel main loop (`src/core/kernel_main.cpp`) still polls network, userland,
console and input, device IRQs stay masked, and the OS virtio drivers spin on
their used rings with `VIRTQ_AVAIL_F_NO_INTERRUPT`. Slices, each validated on
the fast lane and then all ten profiles:
1. Interrupt core: kernel-mode IRQ entry with interrupts enabled only inside
   an idle wait, a per-architecture `PlatformIrqChip` selected at compile time
   behind `IrqChip`, a bounded top half that records the line, and a deferred
   work queue drained by the main loop. The main loop waits for an interrupt
   instead of spinning when no thread is runnable and no deadline is due.
2. Controllers: 8259 (i386, x86_64), GICv2 SPIs (aarch64, arm32), PLIC (rv64,
   rv32), EIOINTC with PCH-PIC (loongarch64), Malta i8259 behind the GT-64120
   (mips, mips64), MPIC (ppc), each with a QEMU probe that observes a real
   device interrupt.
3. Module boundary: an exported IRQ registration service (runtime selection,
   so a virtual boundary) and the paired OS virtio blk / net / rng / input
   drivers completing through it; polling only behind an explicit option.
4. Wait queues woken from IRQ sources: console input, network receive, block.
5. POSIX timers (`timer_create` family), `setitimer`/`getitimer`/`alarm`.
6. Gate evidence: no polling loop in the block/net/input paths on ten
   profiles, clock drift against QEMU wall time, timer interrupt latency.

### WT1.3 `refactor/ext4-layout` (M, header set changes)

Scope: consolidate the 102 `include/ok/fs` ext4/JBD2 headers into
`include/ok/fs/ext4/{layout,extent,dir,alloc,txn,journal,recovery,mount}.hpp`
with private `.inc` files under `src/fs/ext4/`. No behavior change. Appendix A
is the generated starting map. Tests under `tests/host/ext4_*` move with the
headers; hosted ext4 suites and the OS two-boot persistence test must produce
identical results. Can run in parallel with WT1.1; align the ABI bump at merge.

Folded (worktree `refactor/ext4-layout`, paired OS worktree `refactor/ext4-layout`):
- 85 slices are folded into the eight modules, layered by actual dependency
  `journal → layout → extent → alloc → dir → txn → recovery → mount` (Appendix A
  was cyclic); the 20 partials are in `include/ok/fs/ext4/detail/`, because they
  are template class bodies the OS compiles from the SDK. `include/ok/fs/ext4/README.md`
  maps every former header; the two SDK profile marker headers stay standalone.
  `docs/qualification/EXT4_MODULE_LAYOUT.md` records the equivalence checks.
- `src/`, tests, hosted runners, `tools/export_sdk.py` and the OS consumer use the
  module headers; `scripts/check_ext4_layout.py` enforces the layering.
Merge: `feat/mm-general` merged first at ABI 75, so this worktree merges main and
bumps the module ABI to 76; the paired OS worktree consumes SDK 76 and merges
right after the kernel.

### WT1.4 `refactor/user-runtime-split` (M)

Scope: split `src/user/runtime.cpp` (5,876 lines, 101 includes) into
`loader / entry / vm / wait / pty / net / signals` units; collapse the 30
per-architecture `native_signal_{delivery,frame,return}_<arch>.hpp` headers into
one `include/ok/arch/<arch>/signal.hpp` each; move the kernel-resident shell,
file manager and task manager to `src/rescue/`, built only with `kernel_gui`.
Appendix B and C list the files. No behavior change; release image size must
not grow.

Scaffold (merged to main from `refactor/user-runtime-split`):
- `src/apps/` moved to `src/rescue/` with `src/rescue/README.md`; `xmake.lua`
  and `tests/host/native_pty/xmake.lua` updated; headers stay under `include/ok/apps`.
- `src/user/README.md` unit map; `scripts/check_runtime_split.py` ratchet
  (5,876 lines / 101 includes) that fails when `runtime.cpp` grows.

Merged to main from `refactor/user-runtime-split` (tag `p1-user-runtime-split`):
- `runtime.cpp` and its 25 mid-namespace fragments are nine units under
  `src/user/runtime/` (`loader`, `vm`, `wait`, `futex`, `pipe`, `pty`, `net`,
  `signals`, `entry`) plus a 675-line `runtime.cpp`, sharing
  `src/user/runtime/private.hpp`; the map and rules are in `src/user/README.md`.
  Hosted tests compile the units as one translation unit through
  `tests/host/native_runtime_units.hpp`. The ratchet caps `runtime.cpp` at 675
  lines and rejects textual fragments and per-architecture signal pieces.
- The 32 per-architecture signal headers (frame, delivery, return and the MIPS/PPC
  context, readiness, gate and persistent state) are one
  `include/ok/arch/<arch>/signal.hpp` each for `i386`, `x86_64`, `aarch64`,
  `arm32`, `riscv` (rv32 and rv64), `loongarch64`, `mips`, `mips64` and `ppc`.
  This changes the exported header set but not the binary interface, and no
  ObfuscationOS source includes these headers. It merges at module ABI 74;
  the ABI 75 bump of `feat/mm-general` covers it and its ABI history entry
  names the move.
- Hosted runners compile `src/rescue` again (they still listed `src/apps`);
  69 runners pass instead of 58.
- `src/rescue` is compiled only with `kernel_gui` or in debug builds; other
  release builds link `src/rescue/disabled.cpp`, which keeps the interfaces
  (`Kernel` layout and SDK symbols are unchanged) and behaves as never opened.
- Release image `.text` (no `kernel_gui`) against the start of the worktree:
  aarch64 -91K, arm32 -92K, i386 -95K, loongarch64 -103K, mips -113K,
  mips64 -122K, ppc -137K, rv32 -86K, rv64 -76K, x86_64 -104K. The split alone
  grows `.text` by up to 16K (ppc +44K, mostly fill before the 64 KiB-aligned
  exception vectors) because code is no longer inlined across units; dropping
  the rescue applications more than offsets it. All ten release kernels boot to
  `status=ready`.
- Results and ObfuscationEvidence receipts:
  [qualification/USER_RUNTIME_SPLIT.md](qualification/USER_RUNTIME_SPLIT.md).

### P1 review issues (#5–#8)

Four worktrees answer the 2026-10-28 review of the P1 results; WT1.2 (#4) stays
its own worktree.
- `chore/hosted-runners` (#5): every `scripts/test_*.py` is classified and the
  runnable ones run in CI (`scripts/hosted_suite.py`, `xmake hosted-test`,
  `xmake native-probe`); [qualification/HOSTED_RUNNER_SUITE.md](qualification/HOSTED_RUNNER_SUITE.md).
- `fix/mips-signal-resume` (#8): resolved page faults publish MIPS captures; a
  refused handler frame ends only its process;
  [qualification/MIPS_SIGNAL_RESUME.md](qualification/MIPS_SIGNAL_RESUME.md).
- `fix/ext4-node-cache` (#7, paired OS `fix/ext4-node-cache`): both ext4
  providers evict unreachable nodes and the create/data scans are bounded by
  the filesystem; [qualification/EXT4_NODE_CACHE.md](qualification/EXT4_NODE_CACHE.md).
- `feat/linux-personality` (#6, paired OS `feat/linux-personality`): ARG_MAX
  exec arguments, `ENOEXEC` and `#!`, devfs, vfork and the routed Linux calls;
  [qualification/LINUX_PERSONALITY_LIMITS.md](qualification/LINUX_PERSONALITY_LIMITS.md).
Merge: the three smaller branches merge into `feat/linux-personality`, which
bumps the module ABI once to 77 for all four; the paired OS worktree consumes
SDK 77. Merge the kernel PRs in the order #5, #8, #7, #6, then the OS PRs.

## P2: platform and protocols

### WT2.1 `feat/smp-bringup` (XL)

AP start on x86_64 (SIPI), aarch64 (PSCI) and rv64 (SBI HSM) first; per-CPU
run queues in `BasicScheduler`; IPIs; TLB shootdown; lock ranks with debug
checking; per-CPU storage. Other profiles report UP honestly. Exit: 2–4 vCPU
concurrent filesystem/socket/fork stress on three profiles. Depends on WT1.1, WT1.2.

### WT2.2 `feat/tty-jobctl` (L)

termios, N_TTY line discipline, controlling terminal and foreground process
group, SIGTTIN/SIGTTOU/SIGHUP, devpts-style PTY allocation on top of the existing
raw PTY pairs and console lease. Paired OS worktree `feat/getty-login`. Depends
on WT1.2.

### WT2.3 `feat/net-core` (XL)

ICMP, neighbour cache, routing table, general TCP (retransmission, windows,
congestion control), IPv6 with neighbour discovery, socket readiness through the
generic wait queues. Keep packet parsing and checksums as constrained templates;
one virtual boundary per device queue. Paired OS worktree `feat/net-services`.
Depends on WT1.2.

### WT2.4 `feat/storage-durable` (XL)

Default journalled writer (JBD2 ordered mode), write-back through the page
cache, extent tree growth, htree directories, ext4 symbolic links,
`metadata_csum` read and write. Paired OS worktree `fix/mkfs-e2fsprogs`. Exit:
power-cut replay matrix on ten profiles, Linux round trip with `e2fsck -fn`
clean. Depends on WT1.1, WT1.3.

### WT2.5 `feat/module-loader` (L)

ELF relocation for native modules on ten profiles (RELA/REL per architecture),
symbol registry resolution, dependency reference counts, unload refusal while
objects are live. Paired OS worktree `feat/drivers-as-modules`. Depends on WT1.1.

### WT2.6 `feat/display-input-uapi` (L)

`include/ok/uapi/display.h` and `input.h`: dumb buffer allocation, scanout,
vsync events, input event queue, seat lease. Move ramfb, virtio-gpu, PS/2 and
USB HID drivers out of the kernel to OS drivers; keep a text rescue GUI. Paired
OS worktree `feat/display-input-drivers`. Depends on WT1.1, WT2.5.

Scaffold (merged to main from `feat/display-input-uapi`):
- `include/ok/uapi/display.h`, `include/ok/uapi/input.h`: versioned fixed-width
  records for modes, dumb buffers, present/vblank events, input events, device
  info and seat leases; reserved ioctl numbers; `OK_*_UAPI_VERSION` are 0 until
  the first native handler lands.
- `tests/host/display_input_uapi_layout.c` (freestanding, LP64 and ILP32),
  `tests/host/display_input_uapi_tests.cpp`, `scripts/test_display_input_uapi.py`.
Next: kernel display mechanism (`ok::display` buffer objects over the general
allocator), `/dev/display0` and `/dev/input/eventN` descriptors, seat lease owner.

## P3 and later

Kernel work in P3 is support for the OS compositor and loader (shared mappings,
futex/thread completeness, `SCM_RIGHTS` breadth). P4 covers M10 ext4 feature
breadth, M11 physical reference machines and M12 release qualification; each
opens its own worktree from this plan.

## Appendix A: proposed ext4 header consolidation (`refactor/ext4-layout`)

Generated from the current `include/ok/fs/` listing by name pattern; the worktree
refines the mapping by actual dependency, not by name. Target layout is
`include/ok/fs/ext4/<module>.hpp` with one header per module plus private `.inc`
files under `src/fs/ext4/`.

### `ext4/layout.hpp` (5 files)

`ext4.hpp`, `ext4_completed_ownership_birth.inc`, `ext4_fs.hpp`, `ext4_readonly.hpp`, `ext4_times.hpp`

### `ext4/extent.hpp` (19 files)

`ext4_allocated_empty_data_ownership.hpp`, `ext4_allocated_empty_data_plan.hpp`, `ext4_allocated_empty_data_preview.hpp`, `ext4_allocated_empty_data_session_preview.hpp`, `ext4_allocated_empty_data_transaction.hpp`, `ext4_allocated_empty_data_transaction_budget.hpp`, `ext4_allocated_empty_data_witness.hpp`, `ext4_blocks.hpp`, `ext4_direct_read_scope.hpp`, `ext4_first_data_plan.hpp`, `ext4_first_data_transaction.hpp`, `ext4_next_data_plan.hpp`, `ext4_next_data_transaction.hpp`, `ext4_zero_truncate_plan.hpp`, `ext4_zero_truncate_preview.hpp`, `ext4_zero_truncate_session_preview.hpp`, `ext4_zero_truncate_transaction.hpp`, `ext4_zero_truncate_transaction_budget.hpp`, `ext4_zero_truncate_witness.hpp`

### `ext4/dir.hpp` (13 files)

`ext4_directory_initialization_budget.hpp`, `ext4_namespace_completion.hpp`, `ext4_namespace_plan.hpp`, `ext4_namespace_result_history.hpp`, `ext4_namespace_transaction.hpp`, `ext4_namespace_witness.hpp`, `ext4_owned_regular_move.inc`, `ext4_regular_move_completion.hpp`, `ext4_regular_move_plan.hpp`, `ext4_regular_move_transaction.hpp`, `ext4_regular_move_witness.hpp`, `ext4_rename_plan.hpp`, `ext4_rename_transaction.hpp`

### `ext4/alloc.hpp` (24 files)

`ext4_create_plan.hpp`, `ext4_create_transaction.hpp`, `ext4_create_witness.hpp`, `ext4_directory_create_plan.hpp`, `ext4_directory_create_sdk_profile.hpp`, `ext4_directory_create_witness.hpp`, `ext4_free_inode.hpp`, `ext4_inode_lifetime.hpp`, `ext4_orphan_access.hpp`, `ext4_orphan_chain.hpp`, `ext4_orphan_chain_capture.hpp`, `ext4_orphan_directory_qualified_reclaim.hpp`, `ext4_orphan_directory_refs.hpp`, `ext4_orphan_ownership.hpp`, `ext4_orphan_reclaim_capture.hpp`, `ext4_orphan_reclaim_cold.hpp`, `ext4_orphan_reclaim_plan.hpp`, `ext4_orphan_reclaim_witness.hpp`, `ext4_orphan_snapshot.hpp`, `ext4_reclaim_composed_budget.hpp`, `ext4_reclaim_composed_history.hpp`, `ext4_reclaim_history.hpp`, `ext4_reclaim_result_history.hpp`, `ext4_reclaim_route_budget.hpp`

### `ext4/txn.hpp` (7 files)

`ext4_completion.hpp`, `ext4_metadata_transaction.hpp`, `ext4_owned_common_opening.inc`, `ext4_owned_mounted_cursor.inc`, `ext4_owned_transaction.hpp`, `ext4_owned_typed_port.inc`, `metadata_buffers.hpp`

### `ext4/journal.hpp` (13 files)

`ext4_allocated_empty_data_journal.hpp`, `ext4_allocated_empty_data_journal_verify.hpp`, `ext4_namespace_cold_replay.hpp`, `ext4_namespace_journal.hpp`, `ext4_namespace_replay.hpp`, `ext4_orphan_reclaim_journal.hpp`, `ext4_regular_move_journal.hpp`, `ext4_regular_move_replay.hpp`, `ext4_replay_shape.hpp`, `ext4_replay_view.hpp`, `ext4_zero_truncate_journal.hpp`, `jbd2.hpp`, `jbd2_replay.hpp`

### `ext4/recovery.hpp` (5 files)

`ext4_empty_orphan_recovery.hpp`, `ext4_namespace_recovery_binding.hpp`, `ext4_reclaim_recovery_binding.hpp`, `ext4_recovery.hpp`, `ext4_regular_move_recovery.hpp`

### `ext4/mount.hpp` (21 files)

`ext4_mounted_common_opening.inc`, `ext4_mounted_common_opening_budget.hpp`, `ext4_mounted_common_profile.hpp`, `ext4_mounted_metadata.hpp`, `ext4_mounted_namespace_checkpoint.inc`, `ext4_mounted_namespace_history.inc`, `ext4_mounted_original_node_authority.inc`, `ext4_mounted_reclaim_checkpoint.inc`, `ext4_mounted_reclaim_history.inc`, `ext4_mounted_regular_move.inc`, `ext4_mounted_regular_move_startup.inc`, `ext4_mounted_startup_locator.inc`, `ext4_mounted_startup_owner.inc`, `ext4_mounted_startup_port.inc`, `ext4_mounted_startup_workspace.hpp`, `ext4_mounted_typed_budget.hpp`, `ext4_mounted_typed_cold_startup.inc`, `ext4_mounted_typed_cursor.hpp`, `ext4_mounted_typed_publication.inc`, `ext4_mounted_typed_request.inc`, `ext4_mounted_typed_suffix.inc`

Total ext4/JBD2 headers mapped: 107. Unrelated headers kept as they are: `archive.hpp`, `archive_directory_profile.hpp`, `attributes.hpp`, `fat.hpp`, `fat_fs.hpp`, `mode_change.hpp`, `mode_policy.hpp`, `mount_retention.hpp`, `mount_snapshot.hpp`, `mount_snapshot_profile.hpp`, `persistent_sync_profile.hpp`, `simplefs.hpp`, `storage.hpp`, `unix.hpp`, `vfs.hpp`

## Appendix B: proposed user runtime split (`refactor/user-runtime-split`)

The split as implemented is mapped in `src/user/README.md`; this table is the
original proposal.

| Target file | Current sources |
| --- | --- |
| `loader.cpp (ELF admission, argv/auxv, address-space setup)` | new |
| `entry.cpp (native loop, enter_owned, syscall/timer/fault dispatch)` | `runtime.cpp (split)`, `native_lifecycle.hpp`, `native_pipe_write_runtime.hpp`, `native_shared_capture.hpp`, `native_syscall_origin.hpp`, `native_syscall_restart_publication.hpp`, `native_vfs_retirement.hpp` |
| `vm.cpp (mmap/mprotect/munmap/brk, page pools)` | new |
| `wait.cpp (wait4, poll/select, sleep, futex)` | `native_child_wait_runtime.hpp`, `native_poll_mask.hpp` |
| `pty.cpp` | `native_pty_owners.hpp`, `native_pty_runtime.hpp` |
| `net.cpp (UDP/TCP runtime adapters)` | `linux_udp_captured_decode.hpp`, `linux_udp_immediate_access.hpp`, `linux_udp_immediate_operation.hpp`, `linux_udp_operation_body.hpp`, `linux_udp_original_ofd.hpp`, `linux_udp_receive_authority.hpp`, `linux_udp_receive_order.hpp`, `linux_udp_receive_owner_store.hpp`, `linux_udp_receive_owners.hpp`, `linux_udp_receive_runtime.hpp`, `linux_udp_runtime.hpp`, `native_udp_poll_runtime.hpp`, `native_udp_receive_owners.hpp`, `native_udp_receive_runtime.hpp`, `tcp_captured_accept.hpp`, `tcp_immediate_access.hpp`, `tcp_immediate_operation.hpp`, `tcp_immediate_runtime.hpp`, `tcp_original_ofd.hpp` |
| `signals/ (per-architecture frames collapse into arch/<a>/signal.hpp)` | `signals/ (18 files)` |

`include/ok/user/` has 88 headers; the 30 `native_signal_{delivery,frame,return}_<arch>.hpp`
headers collapse into one `include/ok/arch/<arch>/signal.hpp` per architecture.

## Appendix C: kernel-resident applications now under `src/rescue/`

- `src/rescue/file_manager.cpp` (928 lines)
- `src/rescue/shell/filesystem.cpp` (934 lines)
- `src/rescue/shell/network.cpp` (165 lines)
- `src/rescue/shell/status.cpp` (747 lines)
- `src/rescue/shell/storage.cpp` (350 lines)
- `src/rescue/shell/task_manager.cpp` (78 lines)
- `src/rescue/shell.cpp` (2619 lines)
- `src/rescue/task_manager.cpp` (1088 lines)
