# Native execution and persistent-storage validation

Validated locally on 2026-09-19 with GCC 14.2 cross compilers and QEMU system
emulators. `scope=hardware` identifies actual guest CPU execution and device
transactions under QEMU. It does not certify physical machines, hardware SMP
or full POSIX conformance. Hosted models are listed separately.

## Current acceptance

| Area | Result and scope |
| --- | --- |
| Standalone debug kernels | All ten profiles build and boot with 57 debug test points, including explicitly labeled model coverage |
| Native distro debug | All ten profiles: actual `/init`, fork/exec/wait, isolation, guarded pointers, contained faults, timer preemption, pipe EOF/EPIPE and an external-program shell pipeline |
| Native file ABI | All ten profiles: fixed-width/native-endian directory entries, positioned and vectored I/O, zero writes, sparse/truncate semantics, fsync/fdatasync and EBADF/ESPIPE checks |
| Interactive applications | All ten debug profiles: serial shell, external/builtin pipelines, Notes write/read, Tasks, Settings and About |
| Release artifact consumer | x86_64, i386 and MIPS build in a separate source tree, consume copied SDKs via explicit paths, pass UAPI/libc checks and run interactive applications; x86_64/i386 also pass ext4 restart persistence |
| Real block driver | i386, x86_64, arm32, aarch64, rv32, rv64, loongarch64: Virtio reads/writes, FLUSH, restart reads, read-only and absent-device checks; ARM/RISC-V MMIO v1/v2 and recoverable read-error injection have separate probes |
| Native ext4 persistence | Those seven profiles: execute `/init` from ext4, save and sync a note, restart QEMU, read the same bytes, pass `e2fsck -fn` after each boot and verify contents with `debugfs` |
| Architecture probes | The six new backends pass separate user-mode/register/TLS/FP, address-space, protection and timer probes; i386/PPC/MIPS also test supervisor-fault handling |
| Canonical resources | Hosted process-resource and static-dispatch suites pass all ten target profiles; these remain model checks |
| VFS/POSIX host regression | Mount-point protection, opened-directory identity across mounts, component traversal, fork/dup node lifetime, offset/append rules, short vector writes and errno mapping |
| ext4 host regression | 1/2/4 KiB blocks: mutation, reopen, clean fsck, unsupported/unclean/read-only rejection, inode reuse and 32-bit ownership |
| ext4 failure model | 636 injected read, partial-write and flush failures; malformed extents/group metadata, sticky failure state, dirty-marker recovery attempts and retained-unlinked node lifetime |
| Tools | 25 Kernel QEMU runner tests and 39 OS SDK/archive/image/runner tests |

All native profiles are retained: i386, x86_64, aarch64, arm32, rv64, rv32,
loongarch64, mips, mips64 and ppc. SDK checks cover architecture, mode, compiler,
ABI generation 2, artifact hashes and required assembler flags. The OS builds
only its own sources against exported Kernel SDK artifacts.

Logs and generated test disks are intentionally unversioned. The current debug
matrix is recorded under `build/storage-final-debug/` and
`build/storage-final-debug.log` in the Kernel repository; the final per-profile
status is in `results.json`. Isolated release checks are recorded under
`build/storage-final-release/`. Architecture probes
are under `build/storage-final-probes/`; hosted checks use
`build/storage-final-*.log` and `build/ext4-fs-test.log`. OS persistence evidence
is under `build/<arch>/debug/persistence/`, including both serial logs, offline
contents and a verified scratch disk. Reproduction commands are in
[NATIVE_EXECUTION.md](NATIVE_EXECUTION.md), [ext4 storage](modules/ext4-storage.md)
and the OS `docs/STORAGE.md` / `docs/VIRTIO_BLOCK.md`.

## Retained desktop validation

The previous GUI round checked production-renderer output, OS integration,
x86_64 debug/release QEMU frames, and keyboard/serial separation. The light
cool-gray/blue design and screenshots remain in the OS `docs/gui/` directory.
Kernel diagnostics do not overwrite the compositor; the desktop reports
applications without claiming modeled CPU slots are executing hardware cores.

## Remaining limits

- MIPS/MIPS64/PPC userland runs from the embedded archive. Their real block
  transports remain unimplemented; offline ext4 packaging is not persistence.
- ext4 is a bounded non-journalled subset. JBD2 replay, checksums, 64-bit group
  descriptors, directory indexes, rename, hard-link creation and symlink
  traversal are unsupported. Interrupted mutation requires offline fsck.
  Permanent media failure can prevent even a dirty-marker recovery write.
- Native execution and storage currently use the boot CPU; physical-board DMA,
  IOMMU, noncoherent cache maintenance and hardware SMP are unvalidated.
- The retained MIPS CPUs lack hardware NX. LoongArch's strict branch-to-NX probe
  hits a recursive emulator fault on QEMU 8.2.2; the entry guard passes and NX
  bits remain set. See the architecture documents for exact limits.
- GUI application views remain OS-owned prelinked extensions. A user-mode GUI
  protocol and runtime ELF module relocation/loading are still future work.
- General VM allocation, signals, native threads, calibrated clocks/sleeps,
  sockets and full POSIX/Linux binary compatibility remain incomplete.
