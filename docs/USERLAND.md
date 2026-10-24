# Userland Development

This document defines the current downstream user-space contract for the
`0.1.x` line. The contract is intentionally small: it is suitable for tiny
freestanding static programs and ABI tests, not for unmodified Linux
distributions or dynamically linked libc workloads.

## Public UAPI

Downstream code should include:

```c
#include <ok/uapi/syscall.h>
```

This header is C-compatible and exposes:

- `OK_UAPI_VERSION_*` version macros;
- `OK_SYS_*` syscall numbers matching the Linux x86_64 table where the kernel
  has a handler;
- `OK_E*` errno values returned as negative syscall results;
- file, access, memory-map, fcntl, seek, `arch_prctl`, futex, and file-mode
  constants;
- native 0.1.x structs: `ok_timespec`, `ok_timeval`, `ok_iovec`, `ok_stat`,
  `ok_rlimit`, and `ok_sysinfo`.

The roadmap tests compile this header into the kernel test path and assert that
the constants and native struct layouts match the C++ kernel implementation.

## Syscall ABI

All ten native backends use the common syscall number table. The register
contract is:

| Profile | Entry instruction | Number | Arguments 0–5 | Result |
| --- | --- | --- | --- | --- |
| i386 | `int 0x80` | `eax` | `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp` | `eax` |
| x86_64 | `syscall` | `rax` | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` | `rax` |
| aarch64 | `svc #0` | `x8` | `x0..x5` | `x0` |
| arm32 | `svc #0` | `r7` | `r0..r5` | `r0` |
| rv64, rv32 | `ecall` | `a7` | `a0..a5` | `a0` |
| loongarch64 | `syscall 0` | `a7` | `a0..a5` | `a0` |
| mips | `syscall` | `v0` | `a0..a3`, `t0`, `t1` | `v0` |
| mips64 | `syscall` | `v0` | `a0..a5` | `v0` |
| ppc | `sc` | `r0` | `r3..r8` | `r3` |

Successful calls return a non-negative value. Failed calls return `-OK_E*`.
Unknown or intentionally unsupported calls return `-OK_ENOSYS`.
This is the Obfuscation UAPI, not the platform-specific Linux ABI. In
particular, MIPS does not signal errors through `a3`, and PPC does not use
`CR0.SO`. The OS libc shim converts negative syscall results into `errno`.

## Stable For 0.1.x

The following areas are intended to remain stable across 0.1.x:

- syscall number assignments in `ok/uapi/syscall.h`;
- negative errno return convention;
- descriptor constants and memory-map flags exposed by the UAPI header;
- native layouts for `ok_timespec`, `ok_timeval`, `ok_iovec`, `ok_stat`,
  `ok_rlimit`, and `ok_sysinfo`;
- the debug-only `OK_SYS_OK_DEBUG` passthrough syscall used by smoke tests.

## Implemented Syscall Groups

The trusted-buffer kernel/model table handles the calls below. Only the checked
subset listed in [Native execution](NATIVE_EXECUTION.md) is exposed to actual user
programs; internal model support does not imply native syscall support:

- descriptor I/O: `read`, `write`, `pread64`, `pwrite64`, `readv`, `writev`,
  `lseek`, `fsync`, `fdatasync`, `truncate`, `ftruncate`, `close`, `close_range`,
  `dup`, `dup2`, `dup3`, `fcntl`, and `ioctl`;
- paths and metadata: `open`, `openat`, `creat`, `stat`, `lstat`, `fstat`,
  `newfstatat`, `mkdir`, `mkdirat`, `unlink`, `unlinkat`, `rmdir`, `chdir`,
  `fchdir`, `getcwd`, `access`, `faccessat`, `faccessat2`, `getdents`, and
  `getdents64`;
- clocks and identity: `getpid`, `getppid`, `gettid`, uid/gid queries,
  `clock_gettime`, `clock_getres`, `nanosleep`, `clock_nanosleep`,
  `gettimeofday`, `time`, `uname`, `getrlimit`, `prlimit64`, and `sysinfo`;
- startup/runtime probes: `brk`, `mmap`, `mprotect`, `munmap`, `arch_prctl`,
  `futex`, `getrandom`, `set_tid_address`, `set_robust_list`, `rseq`,
  `rt_sigaction`, `rt_sigprocmask`, `sched_yield`, `exit`, `exit_group`, and
  `umask`.

The native runtime implements `fork`, `execve`, `wait4` and `pipe` against the
canonical scheduler resources. `clone`, `kill`, `select` and `poll` remain
unsupported at the user boundary. The internal trusted-buffer table retains
its separate model handlers.

## Native ABI Notes

Native real/effective/saved UID/GID setters and getters are separately
advertised by the strict SDK credential capability. See
[Native process credentials](qualification/NATIVE_CREDENTIALS.md) for authorization, the
single-live-thread restriction and the checked-copy contract.

`ok_stat` is the native 0.1.x metadata layout. Its `type` field uses
`OK_NODE_*`, while `mode` carries Unix-style type and permission bits such as
`OK_MODE_REGULAR`, `OK_MODE_DIRECTORY`, and `OK_MODE_PERMISSION_MASK`.

`uname` currently writes a kernel-native structure rather than a Linux libc
`struct utsname`. Downstream code should prefer `OK_UAPI_VERSION_*` for feature
checks until a public `ok_utsname` layout is frozen.

Pointers, syscall arguments and the OS shim's `off_t` follow the native word
size; fixed-width UAPI integers remain the declared width on ELF32 and ELF64.
Vectored I/O accepts at most 16 checked native-word `ok_iovec` entries and a
4096-byte total transfer. Positioned I/O leaves the shared descriptor offset
unchanged. `fsync` and `fdatasync` synchronize mounted ext4 through device FLUSH;
RAM files have no persistent backing.

The trusted-buffer `mmap` handler allocates bounded kernel memory-map records.
The native boundary independently implements private anonymous `mmap`,
`mprotect` and partial `munmap` through real architecture mappings. It retains
the static ELF loader and eager fork; `brk` remains unavailable. See
[native VM](qualification/NATIVE_VM.md) for the supported protections and resource bounds.

## What Is Still Missing

ObfuscationOS supplies crt0, static linker scripts, libc wrappers, `/init` and
an external-program shell for all ten profiles. Native execution, address-space
switching and timer traps are exercised in QEMU; physical hardware validation
is separate. The retained MIPS CPUs lack hardware NX, and execution uses the
boot CPU rather than hardware SMP. See [Native execution](NATIVE_EXECUTION.md)
for platform limits.

Persistent ext4 roots use real Virtio disks on i386, x86_64, arm32, aarch64,
rv32, rv64 and loongarch64. MIPS, MIPS64 and PPC still need block transports;
they execute userland from the boot archive. The supported ext4 subset has no
journal and requires offline recovery after an interrupted mutation.

Remaining work includes general VM and allocation, signals, threads, calibrated
clocks and blocking sleeps, sockets and broader device interfaces, dynamic
linking, a user GUI protocol, and full POSIX/Linux ABI behavior.
