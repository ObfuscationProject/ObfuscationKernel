# v0.1.0 Roadmap

The `0.1.0` line is a developer-preview kernel contract for downstream
user-space experiments. A release should build tiny static programs against a
documented UAPI, boot them in QEMU, expose useful diagnostics, and preserve
regression coverage. Full POSIX conformance and production hardware support
remain separate goals.

## Current Foundations

- Build and ownership: xmake builds freestanding C++23 for `i386`, `x86_64`,
  `aarch64`, `arm32`, `rv64`, `rv32`, `loongarch64`, `mips`, `mips64` and `ppc`.
  Architecture/mode-specific SDK artifacts let ObfuscationOS link its own
  extensions without embedding or recompiling kernel sources.
- Native userland: all ten profiles have static ELF execution, real privilege
  transitions, address-space switching, timer preemption and guarded user
  copies. ObfuscationOS supplies crt0, libc wrappers, `/init`, an external-program
  shell and system applications. These paths are exercised in QEMU; see
  [Native execution](NATIVE_EXECUTION.md) for each CPU's protection limitations.
- Process resources: the scheduler owns canonical process/thread state,
  address spaces, descriptor tables and open-file descriptions. Native fork,
  execve, wait4 and pipes use these resources, with fault containment, orphan
  collection, EOF/EPIPE and shared file offsets.
- POSIX file operations: RAM and mounted nodes support path lookup, metadata,
  permissions and directories. Native scalar/vectored and positioned I/O,
  truncate/ftruncate and fsync/fdatasync have bounded checked user copies.
  Internal model handlers for memory, signals and futexes remain distinct from
  the supported native syscall subset.
- Persistent storage: generic VFS mount routing and the bounded ext4 writer
  provide hierarchical files through real `BlockDevice` services. OS-owned
  Virtio transports support seven profiles: i386, x86_64, arm32, aarch64, rv32,
  rv64 and loongarch64. Ext4 is non-journalled and requires offline recovery
  after interrupted mutation; [storage notes](modules/ext4-storage.md) define
  supported feature bits and limits.
- Driver and module boundaries: native interfaces provide resources, MMIO,
  IRQ, DMA, wait/work queues, timers and lifecycle services. Model checks cover
  ABI/capability/resource contracts and OKMOD metadata. OS-owned Linux driver
  adaptation and module composition remain separate from runtime code loading.
- GUI: the kernel owns the basic compositor and diagnostics; ObfuscationOS owns
  desktop/session policy and system app views. The current desktop extension
  executes in the kernel; GUI views are not isolated user programs.

## Next Milestones

- User VM: replace fixed backing-pool limits with general physical allocation,
  checked native `mmap`/`mprotect`/`munmap`/`brk`, and eventually copy-on-write.
- POSIX process/runtime behavior: add signals, threads/clone, process groups,
  sessions, calibrated clocks, blocking sleeps and select/poll. Full POSIX
  conformance needs broader semantic and user-space integration tests.
- Storage: add MIPS/MIPS64/PPC PCI host bridge transports, interrupt-driven I/O,
  broader ext4 allocation/directory support, and journal recovery. Current
  filesystems lack rename, hard-link creation and symlink traversal. Physical
  discovery, noncoherent DMA and storage synchronization across CPUs remain
  separate bring-up work.
- Hardware SMP: bring up secondary CPUs and concurrent run queues; modeled
  topology and single-CPU timer preemption do not establish SMP execution.
- ABI compatibility: native `stat`, `uname` and resource layouts and common
  syscall numbering are not each platform's Linux binary ABI. Broader libc,
  dynamic linking and compatibility translation require explicit contracts.
- Networking/devices: implement real NIC data paths, socket syscalls and a
  richer device/TTY interface beyond the existing debug loopback model.
- Runtime extensions: implement relocations, loading and lifecycle enforcement
  for external native code. Metadata validation and static composition alone
  are not runtime kernel-module loading or third-party driver isolation.
- User GUI: define an IPC/shared-buffer protocol and move desktop applications
  into isolated user processes while retaining the kernel's basic diagnostics.

## Release Gate

Keep model tests, standalone kernel boot, native execution and persistence as
separate acceptance results. [Validation](VALIDATION.md) records the current
evidence; a debug marker alone does not establish every hardware behavior.

For each architecture, configure the matching toolchain and run at least:

```sh
xmake f -c -y -m debug -a x86_64
xmake -y -b okernel
xmake test
xmake qemu-test
xmake -y -b kernel-sdk
```

Repeat with all ten profiles for architecture-sensitive changes. Run the
independent native probes and matching OS SDK-consumer/native userland checks
described in [Native execution](NATIVE_EXECUTION.md). Validate release builds
separately because debug test points are disabled there. Use
`xmake qemu-window-test --no-launch` for headless window-command validation.

For filesystem changes, run the hosted ext4 mutation/reopen/e2fsck suite and
the OS's real Virtio block and two-boot ext4 persistence tests on the seven
supported transports. These QEMU tests do not replace physical-machine testing.
