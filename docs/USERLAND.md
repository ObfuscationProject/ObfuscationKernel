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

All four native backends use the common syscall number table. AArch64 uses
`x8` / `x0..x5`, RV64 uses `a7` / `a0..a5`, and LoongArch uses `a7` / `a0..a5`.
The x86_64 Linux-style register ABI is:

| Field | Register |
| --- | --- |
| syscall number | `rax` |
| arg0 | `rdi` |
| arg1 | `rsi` |
| arg2 | `rdx` |
| arg3 | `r10` |
| arg4 | `r8` |
| arg5 | `r9` |
| return | `rax` |

Successful calls return a non-negative value. Failed calls return `-OK_E*`.
Unknown or intentionally unsupported calls return `-OK_ENOSYS`.

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
  `close`, `close_range`, `dup`, `dup2`, `dup3`, `fcntl`, and `ioctl`;
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

`ok_stat` is the native 0.1.x metadata layout. Its `type` field uses
`OK_NODE_*`, while `mode` carries Unix-style type and permission bits such as
`OK_MODE_REGULAR`, `OK_MODE_DIRECTORY`, and `OK_MODE_PERMISSION_MASK`.

`uname` currently writes a kernel-native structure rather than a Linux libc
`struct utsname`. Downstream code should prefer `OK_UAPI_VERSION_*` for feature
checks until a public `ok_utsname` layout is frozen.

The current `mmap` path allocates bounded kernel memory-map records and is
sufficient for startup probes. It does not yet install architecture page-table
entries for arbitrary user processes.

## What Is Still Missing

ObfuscationOS now supplies crt0, a static linker script, libc wrappers, `/init`
and an external-program shell. Four architecture backends execute those programs
with real protection and timer traps. Remaining work includes general VM and
allocation, signals, device-backed persistent storage, dynamic linking, a user
GUI protocol, and full POSIX/Linux ABI behavior. The six other kernel profiles
retain model/boot support and need native execution backends.
