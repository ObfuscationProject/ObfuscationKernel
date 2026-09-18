# Execution and desktop validation snapshot

Validated locally on 2026-09-18 using GCC 14.2 cross toolchains and QEMU system
emulators. Hardware scope here means actual guest CPU execution under QEMU;
these results do not certify physical machines or full POSIX conformance.

| Area | Result and scope |
| --- | --- |
| Standalone debug kernels | All ten profiles build and boot; 57 debug test points per profile, including explicitly labeled model coverage |
| Native distro debug | x86_64, aarch64, rv64, loongarch64: real init, fork isolation, exec, wait, pipes, guarded pointers, contained faults, timer preemption and external shell pipeline |
| Native distro release | All four profiles: serial shell, external and builtin pipelines, Notes write/read, Tasks, Settings and About |
| Native architecture probes | Real privilege entry/return, register/FP/TLS preservation, address-space changes, protection checks and timer interruption; AArch64 tested at EL1 and EL2 boot |
| Canonical resource models | All ten architecture profiles pass process-resource and static-dispatch host checks |
| Kernel services | GUI input/window/tick and idle-runtime boundaries preserve the selected process identity and blocked state |
| Archive/VFS | Large readonly files, bounded parsing, malformed input rejection and backing-buffer independence |
| Python runners/tools | 25 Kernel QEMU runner tests and 25 OS artifact/archive/runner tests |
| SDK consumer | Copied SDK in an independent temporary directory passes distro linkage, QEMU native acceptance, UAPI, libc, Linux-compat archive build and EXT4 offline packaging |
| Incremental archive | Changing app bytes changes the embedded archive and linked image; restoring the app reproduces the original archive |
| GUI | Production renderer, OS integration checks, final x86_64 debug/release QEMU frame inspection and keyboard/serial separation check |

The final display-only changes were additionally checked in the x86_64 standalone
Kernel and OS framebuffer: diagnostics no longer overwrite the compositor, the
login card is intact, and the desktop reports applications rather than claiming
that modeled CPU slots are executing hardware cores.

Local logs remain in `build/final-debug`, `build/final-release`,
`build/final-release-ui`, `build/final-artifacts`, `build/native-tests`, and
`build/final-{process-resources,static-dispatch,kernel-services}.log`.
The OS holds final GUI frames in `docs/gui/screenshots` and capture/reproduction
tools in `tests/gui`. Build logs and binaries are intentionally not versioned.

## Explicit remaining limits

- Six Kernel profiles retain boot/model support and report native execution as
  unsupported. Hardware SMP is not implemented; native threads use CPU 0.
- LoongArch's strict branch-to-NX probe fails on QEMU 8.2.2 because the emulator
  recursively faults before exception entry. The entry guard passes and NX bits
  remain set. See [LoongArch notes](arch/loongarch64.md) for reproduction and impact.
- Executables come from an embedded readonly archive. Attached disks and offline
  image checks do not implement real block-device I/O or reboot persistence.
- GUI application views remain OS-owned prelinked extension code. Native shell
  processes are isolated; GUI application code is not yet a user-mode GUI client.
- General VM allocation, signals, complete POSIX/Linux ABI behavior, runtime ELF
  module relocation and broad Linux driver compatibility remain future work.

See [native execution](NATIVE_EXECUTION.md) for limits and reproducible commands.
