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

The implemented public syscall ABI is the x86_64 Linux-style register ABI:

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

The kernel currently handles:

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

`clone`, `fork`, `execve`, `wait4`, `kill`, `pipe`, `select`, and `poll` are
reserved and registered, but they currently report `-OK_ENOSYS` from the
single-process syscall profile. The internal process manager has roadmap tests
for create/fork/exec/wait state; that model is not exposed as a stable syscall
contract yet.

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

To make downstream user-space development feel complete rather than merely
testable, the project still needs:

- a minimal crt0 and linker script for static freestanding user programs;
- a small libc or syscall wrapper library built on `ok/uapi/syscall.h`;
- a debug-shell command or loader path that accepts a user ELF from a mounted
  filesystem and runs it through a real user-mode entry;
- architecture page tables and trap-return paths for true user/kernel
  isolation;
- public ABI translations for Linux-compatible `stat`, `utsname`, directory
  entries, signals, process creation, and pipes.
