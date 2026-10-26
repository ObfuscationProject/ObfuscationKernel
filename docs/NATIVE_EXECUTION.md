# Native execution and minimal userland

## Scope

The kernel and distribution retain ten architecture profiles. All ten have
native user execution backends and OS crt/syscall implementations:

| Profile | User privilege / translation | Preemption source |
| --- | --- | --- |
| i386 | CPL3 / PAE with NX, QEMU `-cpu max` | PIC/PIT |
| x86_64 | CPL3 / four-level paging | PIC/PIT |
| aarch64 | EL0 / TTBR0, 4 KiB pages | GICv2 physical timer |
| arm32 | USR / ARMv7 short descriptors | GICv2 physical timer |
| rv64 | U-mode / Sv39, M-mode kernel | CLINT machine timer |
| rv32 | U-mode / Sv32, M-mode kernel | CLINT machine timer |
| loongarch64 | PLV3 / software-filled TLB | CPU timer |
| mips | User KSU / software-filled TLB, 24Kf | CP0 Count/Compare |
| mips64 | User KSU / software-filled TLB, MIPS64R2-generic | CP0 Count/Compare |
| ppc | BookE PR / software-filled TLB, e500v2 | DEC |

The i386 profile requires a modern 32-bit CPU with PAE, NX and FXSR; it does not
claim original 80386 hardware compatibility. The retained MIPS CPUs do not
implement RI/XI: writable/executable mappings are rejected by policy, but they
cannot provide hardware NX. PPC uses the existing BookE/SPE CPU and a soft-float
C ABI, preserving the real SPE context. See the individual architecture notes.
Hardware execution below means actual privileged/user instructions under QEMU,
not validation on physical machines. The backends execute on the boot CPU;
modeled CPU topology is not hardware SMP.

## Ownership and execution

`SchedulerState` owns the only process/thread table. Each process owns its native
address-space handle and backing-page list; each thread owns the saved integer,
floating-point and TLS state. See [process resources](modules/process-resources.md).
The native runtime loop in `src/user/runtime/entry.cpp` (the runtime's units are
mapped in [src/user/README.md](../src/user/README.md)) selects runnable native threads from
that table, calls the architecture-selected `execution::enter_owned` with an active [entry stack lease](qualification/NATIVE_ENTRY_STACKS.md), and handles the
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

The loader accepts static ELF32/ELF64 ET_EXEC files with the native class, byte order and architecture with separate
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

The standalone kernel's RAM-backed Virtio model remains isolated from real media.
ObfuscationOS owns a real Virtio block driver and an optional boot filesystem hook.
A supported clean ext4 image replaces the archive root before `/init` is loaded;
executables and subsequent file I/O then come from the attached block device.
Missing devices and non-ext4 disks retain the archive rescue root. An invalid or
unsupported ext4 volume fails startup without formatting or modifying the disk.
See [ext4 storage](modules/ext4-storage.md) for the bounded writer and recovery policy.

## Supported native syscall subset

The native boundary supports fork, execve, wait4, pipe, exit/exit_group; scalar
and vectored file I/O, positioned reads/writes, fsync/fdatasync, truncate/ftruncate, descriptor duplication/closing and the implemented fcntl operations;
path lookup/creation/removal, metadata and directory enumeration; cwd, identity,
umask, uname, sysinfo, getrlimit, clock reads, getrandom and sched_yield.
Transfers are capped at 4096 bytes per call and may return short counts. Vectored
I/O accepts up to 16 checked native-word iovecs; offsets and pointer fields follow
the target word size, while fixed-width UAPI integers retain their declared width.
The code's whitelist is authoritative. The syscall numbers are this project's
common UAPI (based on x86_64 Linux numbering), including on the other CPUs;
this is not Linux binary compatibility.

Private and shared anonymous `mmap`, eager private file `mmap`, `mprotect` and
partial `munmap` now modify native page tables. Shared anonymous frames survive
fork by reference; private pages use eager copies. Protection faults and
transactional failure rollback remain supported.
See [bounded native VM](qualification/NATIVE_VM.md) for supported flags and fixed limits.
The [object pathname resolver](PATH_RESOLUTION.md) also serves cwd-relative exec,
RAM symbolic links and search checks; ext4 symbolic-link creation is unsupported.

`brk`, signals, general ioctl, sockets, futexes, clone/threads,
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
python3 scripts/test_native_i386.py --toolchain toolchains/i386-elf
python3 scripts/test_native_arm32.py
python3 scripts/test_native_rv32.py
python3 scripts/test_native_mips.py --arch mips
python3 scripts/test_native_mips.py --arch mips64
python3 scripts/test_native_ppc.py --toolchain toolchains/powerpc-eabi
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
readonly buffers and contained user faults, orphan collection and exit status,
overwrite/EOF semantics, positioned/vectored I/O, truncate and synchronization. `OK_USERLAND scope=hardware` covers
init and an external-program pipeline. Standalone `qemu_test.py`/`xmake qemu-test`
separately exercises the complete debug kernel model suite on all ten profiles.

The native shell receives serial console input only. Desktop keyboard events go
to the GUI, so editing a note cannot also execute text in the background shell.
To exercise interactive external commands, builtin pipelines and CLI system apps
in either mode, run `scripts/test_interactive_userland.py` in ObfuscationOS with
`--arch`, `--qemu`, `--kernel`, `--rootfs` and `--log` paths. It waits for the
actual shell prompt and desktop startup, then executes and checks each command.
