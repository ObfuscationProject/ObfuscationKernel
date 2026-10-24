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
- CI caches the SysToolchain package per architecture and exports the SDK
  artifact after `xmake test`.
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

### WT1.3 `refactor/ext4-layout` (M, header set changes)

Scope: consolidate the 102 `include/ok/fs` ext4/JBD2 headers into
`include/ok/fs/ext4/{layout,extent,dir,alloc,txn,journal,recovery,mount}.hpp`
with private `.inc` files under `src/fs/ext4/`. No behavior change. Appendix A
is the generated starting map. Tests under `tests/host/ext4_*` move with the
headers; hosted ext4 suites and the OS two-boot persistence test must produce
identical results. Can run in parallel with WT1.1; align the ABI bump at merge.

### WT1.4 `refactor/user-runtime-split` (M)

Scope: split `src/user/runtime.cpp` (5,876 lines, 101 includes) into
`loader / entry / vm / wait / pty / net / signals` units; collapse the 30
per-architecture `native_signal_{delivery,frame,return}_<arch>.hpp` headers into
one `include/ok/arch/<arch>/signal.hpp` each; move the kernel-resident shell,
file manager and task manager to `src/rescue/`, built only with `kernel_gui`.
Appendix B and C list the files. No behavior change; release image size must
not grow.

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
