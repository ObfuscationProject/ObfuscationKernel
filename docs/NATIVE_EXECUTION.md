# Native execution and minimal userland

## Scope

The kernel retains ten architecture profiles and standalone boot tests. Actual
user execution is implemented on the four current distribution architectures:

| Profile | User privilege / translation | Preemption source |
| --- | --- | --- |
| x86_64 | CPL3 / four-level paging | PIC/PIT |
| aarch64 | EL0 / TTBR0, 4 KiB pages | GICv2 physical timer |
| rv64 | U-mode / Sv39, M-mode kernel | CLINT machine timer |
| loongarch64 | PLV3 / software-filled TLB | CPU timer |

`i386`, `arm32`, `rv32`, `mips`, `mips64` and `ppc` keep their boot, platform and
model coverage; their native execution API explicitly reports unsupported.
Hardware execution below means actual privileged/user instructions under QEMU,
not validation on physical machines. The backends execute on the boot CPU;
modeled CPU topology is not hardware SMP.

## Ownership and execution

`SchedulerState` owns the only process/thread table. Each process owns its native
address-space handle and backing-page list; each thread owns the saved integer,
floating-point and TLS state. See [process resources](modules/process-resources.md).
The native runtime in `src/user/runtime.cpp` selects runnable native threads from
that table, calls the architecture-selected `execution::enter`, and handles the
returned syscall, timer or fault. There is no virtual call in architecture entry
or a duplicate native process table. The existing scheduler policy remains a
constrained template; native dispatch currently uses a simple bounded round-robin
scan. Kernel service/desktop operations use POSIX context zero.

A syscall crosses a checked copying boundary. User addresses are validated against
all pages in the requested range; the trusted-buffer kernel syscall table never
receives unchecked user buffers from this runtime. Invalid pointers return EFAULT.
Faulting user programs exit and can be reaped without terminating the kernel.
Fork eagerly copies user pages, retains shared open-file descriptions and resumes
with distinct return values. Exec stages and validates the complete new image
before replacement. Wait blocks and consumes a zombie once. Pipes implement
blocking transfer, EOF, EPIPE and endpoint-close wakeup.
Children of an exiting process are reparented to the kernel service reaper
(parent zero), which automatically collects exited orphans at native-loop
boundaries. Ordinary exit codes retain their low eight bits; fault termination
is represented separately as a signal status.

The loader accepts static, architecture-matching ET_EXEC files with separate
read/execute and writable segments. It builds argc/argv/envp and a small auxiliary
vector on an aligned user stack with a guard page. W+X segments, malformed ranges,
non-executable entry points and oversized images are rejected. Current bounds:
16 address spaces, 128 pages per process, a 4 MiB shared backing pool, a 128 KiB
ELF input buffer, 64 KiB stacks, and up to 16 argument/environment strings each.
These are bring-up limits, not a general physical-memory allocator or COW VM.

## Distribution and boot archive

ObfuscationOS links only its own extension/archive objects with the exported
Kernel SDK. `boot-archive` builds a deterministic OKAR blob from the staged rootfs;
the SDK boot hook exposes it to the kernel's generic readonly archive mounter.
`/init` is a real user ELF. Debug init executes `posix-check`, runs
`oksh -c "hello | cat"`, then starts the interactive shell. Release init starts
the shell directly. OS crt0, libc wrappers, applications and linker layout remain
in the OS repository.

The current virtio block object is RAM-backed modeling. The attached QEMU disk
and offline SimpleFS/EXT4 packaging checks do not prove real device-backed rootfs
I/O. Executable bytes currently come from the immutable embedded archive; writable
VFS files and Notes are temporary and disappear on reboot.

## Supported native syscall subset

The native boundary supports fork, execve, wait4, pipe, exit/exit_group; scalar
file I/O, descriptor duplication/closing and the implemented fcntl operations;
path lookup/creation/removal, metadata and directory enumeration; cwd, identity,
umask, uname, sysinfo, getrlimit, clock reads, getrandom and sched_yield.
Transfers are capped at 4096 bytes per call and may return short counts.
The code's whitelist is authoritative. The syscall numbers are this project's
common UAPI (based on x86_64 Linux numbering), including on the other CPUs;
this is not Linux binary compatibility.

Memory mapping/brk, signals, general ioctl, sockets, futexes, clone/threads,
dynamic linking and other calls return ENOSYS at the native boundary even where
internal model handlers exist. Clock reads currently count 10 ms user timer
interrupts; they are not calibrated wall-clock time and do not advance while
idle. Full POSIX conformance remains unfinished. The OS GUI applications are
prelinked extension views; their GUI code has not moved into isolated processes.

## Reproduce acceptance

First run the independent architecture probes from the Kernel repository:

```sh
python3 scripts/test_native_x86_64.py --toolchain ../SysToolchain/build/local-sdk-x86_64
python3 scripts/test_native_aarch64.py --toolchain toolchains/aarch64-elf
python3 scripts/test_native_rv64.py
python3 scripts/test_native_loongarch64.py
```

These exercise real syscall/return, timer interruption, address-space switching,
register preservation, protection faults and guarded copies. See individual
[architecture notes](arch/) for exact coverage and platform assumptions.
LoongArch NX fault recovery has an emulator limitation documented in its notes;
an entry guard is not hardware branch-to-NX coverage.

For each distribution architecture, export its debug Kernel SDK, then build
`distro_image` and `rootfs-simplefs` in ObfuscationOS using the matching toolchain.
From the OS repository, for example:

```sh
python3 scripts/qemu_distro.py --arch x86_64 --qemu /usr/bin/qemu-system-x86_64 \
  --kernel build/x86_64/debug/distro/kernel.bin \
  --rootfs build/x86_64/debug/rootfs.simplefs.img --fs simplefs \
  --require-native --timeout 30
```

`--require-native` requires all of the desktop completion marker, native timer
preemption, and userland acceptance. `OK_POSIX scope=hardware` covers real
fork/exec/wait, fork isolation, pipe transfer/EOF/EPIPE, invalid user buffers,
readonly buffers and contained user faults. `OK_USERLAND scope=hardware` covers
init and an external-program pipeline. Standalone `qemu_test.py`/`xmake qemu-test`
separately exercises the complete debug kernel model suite on all ten profiles.

The native shell receives serial console input only. Desktop keyboard events go
to the GUI, so editing a note cannot also execute text in the background shell.
To exercise interactive external commands, builtin pipelines and CLI system apps
in either mode, run `scripts/test_interactive_userland.py` in ObfuscationOS with
`--arch`, `--qemu`, `--kernel`, `--rootfs` and `--log` paths. It waits for the
actual shell prompt and desktop startup, then executes and checks each command.
