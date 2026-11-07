# Native execution and persistent-storage validation

Validated locally on 2026-09-19 with GCC 14.2 cross compilers and QEMU system
emulators. `scope=hardware` identifies actual guest CPU execution and device
transactions under QEMU. It does not certify physical machines, SMP on physical
CPUs or full POSIX conformance. Hosted models are listed separately.

## Current acceptance

| Area | Result and scope |
| --- | --- |
| Standalone debug kernels | All ten profiles build and boot with 57 debug test points, including explicitly labeled model coverage |
| Native distro debug | All ten profiles: actual `/init`, fork/exec/wait, isolation, guarded pointers, contained faults, timer preemption, pipe EOF/EPIPE and an external-program shell pipeline |
| Native file ABI | All ten profiles: fixed-width/native-endian directory entries, positioned and vectored I/O, zero writes, sparse/truncate semantics, fsync/fdatasync, rename/link, retained cwd/dirfd after directory moves and errno checks |
| Interactive applications | All ten debug profiles: serial shell, external/builtin pipelines, Notes write/read, Tasks, Settings and About |
| Release artifact consumer | x86_64 and PPC build in a separate source tree, consume copied ABI 3 SDKs via explicit paths, pass UAPI/libc checks, run native POSIX acceptance and interactive applications, and pass ext4 rename/link restart persistence |
| Real block driver | All ten profiles: Virtio reads/writes, FLUSH, restart reads, read-only and absent-device checks; ARM/RISC-V MMIO v1/v2 and recoverable read-error injection have separate probes |
| Native ext4 persistence | All ten profiles: execute `/init` from ext4, save/rename/link/sync, restart QEMU, verify files and renamed directories, pass `e2fsck -fn` after each boot and check shared inode identity/link counts with `debugfs` |
| Architecture probes | The six new backends pass separate user-mode/register/TLS/FP, address-space, protection and timer probes; i386/PPC/MIPS also test supervisor-fault handling |
| Canonical resources | Hosted process-resource and static-dispatch suites pass all ten target profiles; these remain model checks |
| VFS/POSIX host regression | Mount-point protection, directory ancestry/replacement, opened-directory identity, cwd/dirfd across rename/fork, retained unlinked/replaced nodes, RAM capacity reuse, sticky and ancestor search permissions, held-directory access and errno mapping |
| ext4 host regression | 1/2/4 KiB blocks: mutation, reopen, clean fsck, rename/replacement/hard links, directory topology, unsupported/unclean/read-only rejection, inode reuse and 32-bit ownership |
| ext4 failure model | 1,776 injected read, partial-write and flush failures; partial rename/link quarantine, logical extent exhaustion, malformed metadata, dirty-marker recovery and retained-unlinked lifetime |
| Tools | 25 Kernel QEMU runner tests and 39 OS SDK/archive/image/runner tests |
| SMP (QEMU TCG) | x86_64, aarch64 and rv64 at 2 and 4 vCPUs: secondary start, TSC sync (x86_64), user TLB tags, hotplug test point, device lines on every CPU, kernel sections and atomic futex words under the OS gates below; see [SMP_BRINGUP](qualification/SMP_BRINGUP.md) |

All native profiles are retained: i386, x86_64, aarch64, arm32, rv64, rv32,
loongarch64, mips, mips64 and ppc. SDK checks cover architecture, mode, compiler,
ABI generation 3, artifact hashes and required assembler flags. The OS builds
only its own sources against exported Kernel SDK artifacts.

Logs and generated test disks are intentionally unversioned. The current debug
matrix is recorded under `build/names-storage-debug/` and
`build/names-matrix-final.log` in the Kernel repository; the final per-profile
status is in `results.json`. Isolated release checks are recorded under
`build/names-isolated-release/`. Architecture probes
are under `build/storage-final-probes/`, with the new PPC boot probes under
`build/pci-posix-round/`; hosted checks use
`build/names-*.log` and `build/posix-naming-host-all.log`. OS persistence evidence
is under `build/<arch>/debug/names-persistence/`, including both serial logs, offline
contents and a verified scratch disk. Reproduction commands are in
[NATIVE_EXECUTION.md](NATIVE_EXECUTION.md), [ext4 storage](modules/ext4-storage.md)
and the OS `docs/STORAGE.md` / `docs/VIRTIO_BLOCK.md`.

## SMP validation

Kernel lanes and runners (this repository):

- `xmake test --qemu_smp=N` (`scripts/qemu_test.py --smp N`, N = 2 and 4 on
  x86_64, aarch64 and rv64; the other seven profiles must report UP): the
  hardware `OK_SMP` line, `tsc=` with one verified `OK_TSC_SYNC` line per
  secondary on x86_64, `OK_SMP_TLB` with an allowed mode, and
  `OK_SMP_HOTPLUG_BOOT` when printed (`docs/testing/qemu.md`). The debug
  options `x86_tsc_skew_test` (with `--tsc-skew-test`) and
  `smp_hotplug_test` add the skewed-TSC and the hotplug-cycle checks.
- Native probes on two CPUs: `scripts/test_native_x86_64.py --smp 2`
  (`OK_NATIVE_SMP`, `OK_NATIVE_TLB`, `OK_NATIVE_IOAPIC`),
  `test_native_aarch64.py` (two CPUs by default; `OK_NATIVE_SMP`,
  `OK_NATIVE_TLB`, `OK_NATIVE_GIC cpus=2`, at EL1 and EL2) and
  `test_native_rv64.py` (always two harts; `RV64_NATIVE` start, TLB tags,
  device routing and `retarget_away`).
- Hosted model suites (scope=model, host threads or modelled CPUs):
  `test_smp_locks.py`, `test_run_queues.py`, `test_cpu_topology.py`,
  `test_kernel_entry_stack_pool.py`, `test_tsc_sync.py`, `test_tlb_tags.py`,
  `test_futex_word.py`, `test_native_futex_operations.py`,
  `test_irq_wait_time.py`, `test_clock_counter.py`, `test_realtime_clock.py`,
  `test_kernel_section_protocol.py`, `test_ext4_gate_concurrency.py`,
  `test_linux_file_io_equivalence.py` and `test_native_exec_eviction.py`. The
  hosted suite classifies and runs them.

OS gates (paired worktree `wt/os-smp-bringup`, debug SDK of this tree):

- `scripts/qemu_distro.py --smp N`: native boot and acceptance; on the SMP
  profiles the `OK_SMP_IRQS` report must show bound device lines on at least
  two online CPUs.
- `scripts/test_smp_stress.py --smp N`: the static musl stress probe on
  scratch ext4 (`SMP_STRESS_PASS files=480 messages=960 forks=480`), user
  entries on every CPU, and with `--smp-sections on` (default) the
  `OK_SMP_KERNEL` family floors, overlap (`concurrent`, `witnessed`,
  `max_parallel >= 2`) and zero debug assertion counters; `--smp-sections off`
  requires that no `OK_SMP_KERNEL` line appears. `--require-hotplug` (a
  `smp_hotplug_test` kernel) requires a final `OK_SMP_HOTPLUG` with cycles
  and no failures. The receipt records QEMU's TCG thread mode.
- `scripts/test_futex_smp_atomic.py --smp N`: FUTEX_WAKE_OP races against
  user atomics on another CPU and a copy-on-write word after fork
  (`FUTEX_SMP_ATOMIC_PASS`).

<!-- FINAL: integrated gate totals and the ObfuscationEvidence receipt directory. -->

## Retained desktop validation

The previous GUI round checked production-renderer output, OS integration,
x86_64 debug/release QEMU frames, and keyboard/serial separation. The light
cool-gray/blue design and screenshots remain in the OS `docs/gui/` directory.
Kernel diagnostics do not overwrite the compositor; the desktop reports
applications without claiming modeled CPU slots are executing hardware cores.

## Remaining limits

- ext4 is a bounded non-journalled subset. JBD2 replay, checksums, 64-bit group
  descriptors, directory indexes and symlink traversal are unsupported.
  Interrupted mutation requires offline fsck.
  Rename is serialized in memory but not power-loss atomic. Permanent media
  failure can prevent even a dirty-marker recovery write.
- Mount points and their ancestors cannot be renamed. Retained directories that
  are removed/replaced/covered are unreachable by pathname; relative operations
  and getcwd return ENOENT. Reconstructed paths have a 95-byte limit.
- x86_64, aarch64 and rv64 run native threads on two to four QEMU vCPUs;
  kernel work other than the admitted kernel sections stays under one kernel
  lock (`docs/qualification/SMP_BRINGUP.md`); the other profiles run on the
  boot CPU. x86 PCIDs and the `IA32_TSC_ADJUST` path are not exercised under
  TCG. Physical-board DMA, IOMMU, noncoherent cache maintenance, SMP on
  physical CPUs and physical CPU hotplug are unvalidated.
- The retained MIPS CPUs lack hardware NX. LoongArch's strict branch-to-NX probe
  hit a recursive emulator fault on the QEMU 8.2.2 used for that probe; the entry
  guard passes and NX bits remain set. SysToolchain now pins QEMU 10.2.0, and the
  probe has not been rerun on it. See the architecture documents for exact limits.
- GUI application views remain OS-owned prelinked extensions. A user-mode GUI
  protocol and runtime ELF module relocation/loading are still future work.
- General VM allocation, signals, native threads, calibrated clocks/sleeps,
  sockets and full POSIX/Linux binary compatibility remain incomplete.
