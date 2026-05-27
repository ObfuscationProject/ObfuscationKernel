# v0.1.0 Roadmap

The `0.1.0` version line is a developer-preview kernel contract for downstream
user-space experiments. It is not a production POSIX kernel yet. A stable
`v0.1.0` release should mean that users can build tiny static programs against a
documented syscall/UAPI surface, boot the debug kernel in QEMU, inspect failures
through the shell and GUI, and rely on regression tests to catch ABI drift.

## Release Contract

The current release contract is:

- bootable QEMU images for every configured architecture profile;
- a frozen public C UAPI header for syscall numbers, errno values, flags, and
  simple native ABI structs under `include/ok/uapi/`;
- x86_64 Linux-style syscall dispatch with six register arguments and negative
  errno returns;
- RAM-VFS-backed file, directory, permission, clock, identity, memory-map, TLS,
  futex, random, and process-exit syscall smoke coverage;
- scheduler-visible kernel, driver, module, shell, file-manager, task-manager,
  and user-process records;
- documented debug-shell workflows for storage, networking, process inspection,
  GUI surfaces, and power actions;
- roadmap tests that boot through `kernel_main` and emit `OK_TEST_PASS` with
  module, VM, process, VFS, Linux ABI, driver ABI, GUI, storage, networking, SMP,
  interrupt, and preemption markers.

## Implemented Foundations

- Build and boot: xmake builds `okernel` and `okernel_image`; QEMU runners cover
  `i386`, `x86_64`, `aarch64`, `arm32`, `rv64`, `rv32`, `loongarch64`, `mips`,
  `mips64`, and `ppc`.
- User-space ABI: syscall numbers match the Linux x86_64 table where the kernel
  has handlers; `ok/uapi/syscall.h` is the public downstream header for 0.1.x.
- POSIX facade: the current service supports bounded descriptors, VFS-relative
  paths, directory reads, metadata, credentials, clocks, `brk`, `mmap`,
  `mprotect`, `munmap`, TLS setup, futex no-op/wake behavior, `getrandom`, and
  process exit.
- Process model: tests cover ELF metadata loading, simulated user-mode entry,
  process create/fork/exec/wait/exit state, credentials, address-space IDs, and
  shell-launched GUI user-process isolation.
- Storage: RAM VFS has Unix-style metadata and permissions; SimpleFS gives a
  block-backed writable path; EXT4 can validate superblocks and read raw blocks.
- Drivers: built-in drivers become `drv:*` scheduler-visible daemons; the
  native driver ABI includes resources, MMIO, IRQ, DMA mappings, wait queues,
  work queues, timers, slab allocation, and kernel-thread registry primitives.
- Modules: `ModuleManager` validates ABI version, capability masks, dependency
  order, required services, resource budgets, restart policies, kernel-process
  execution, OKMOD metadata, Linux `.ko` metadata, and Linux ABI snapshots.
- GUI: `kernel-gui` is a restartable module process exposing `gui.compositor`
  and `gui.desktop`; the desktop service now covers windows, scanout, cursor,
  shared buffers, clipboard, and routed input.

## Remaining Blockers For A Stable Downstream v0.1.0

- Real user execution: architecture trap-return paths and hardware page tables
  still need to replace the simulated user-mode gateway before arbitrary
  downstream binaries can be run as isolated ring/user-mode programs.
- Userland SDK: there is now a public UAPI header, but a tiny libc/sysroot,
  crt0, linker script, and example static programs are still needed.
- ABI structs: `stat`, `uname`, and some resource structs are native
  ObfuscationKernel layouts, not Linux libc layouts. The native layouts are
  documented for 0.1.x, but a compatibility translation layer is still needed
  before stock Linux binaries can be treated as supported.
- Process syscalls: the scheduler and process manager can model fork/exec/wait,
  but `clone`, `fork`, `execve`, `wait4`, `kill`, `pipe`, `select`, and `poll`
  are still explicit `-ENOSYS` syscall handlers in the single-process profile.
- Persistent mounts: SimpleFS is flat and block-backed, while EXT4 remains
  read-only at the block/superblock layer. VFS mount routing and writable
  hierarchical disk filesystems remain open.
- Networking: the debug loopback stack is useful for bring-up, but socket
  syscalls and real NIC transmit/receive paths are not v0.1.0-stable.
- Runtime scheduling: CPU accounting and per-CPU state are covered, but true
  concurrent SMP run queues, timer-derived runtime accounting, and preemptive
  user scheduling are still roadmap items.
- External modules and drivers: module and driver ABIs are covered by metadata
  and shim tests, but loading, relocating, signing, and isolating third-party
  modules at runtime is not complete.

## Recent Roadmap Changes

Recent commits moved the roadmap in three important directions:

- `491038b` and `a1421ca` added module ABI/resource enforcement, OKMOD/Linux
  `.ko` metadata parsing, Linux ABI snapshots, driver runtime primitives, and
  the GUI desktop service boundary. Documentation is now covered by
  `docs/modules/modules.md`, `docs/modules/drivers.md`,
  `docs/modules/gui.md`, and `docs/USERLAND.md`.
- `224c8be`, `f47c1fe`, and `a8d9134` stabilized scheduler-visible GUI
  applications, task-manager CPU/process sampling, GUI shell cleanup, kernel
  daemon restart behavior, and per-CPU accounting. The corresponding behavior is
  documented in the scheduler, GUI, debug-shell, QEMU, and driver module notes.
- `21d0804`, `53d0776`, and `263124b` added GUI scroll conventions, polling
  idle hooks, non-debug GUI builds, and QEMU window launcher behavior. Those are
  covered by the GUI, debug-shell, testing, and build docs.

## Release Gate

Before tagging `v0.1.0`, run at minimum:

```sh
xmake f -c -m debug -a x86_64
xmake toolchain-check
xmake -y -b okernel
xmake test
xmake qemu-test
```

For an architecture-sensitive release candidate, repeat `xmake qemu-test -a
<arch>` for every bootable profile listed in `README.md`, and run
`xmake qemu-window-test --no-launch` where the environment is headless.
