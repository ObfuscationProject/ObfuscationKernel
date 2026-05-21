# POSIX Interface

The syscall namespace uses POSIX/Linux-compatible numbers for common calls such
as `read`, `write`, `open`, `close`, `getpid`, `fork`, `execve`, and `exit`.

The current kernel exposes a bounded POSIX service over the RAM VFS. It is not a
complete POSIX implementation yet, but it provides enough real file-descriptor,
time, identity, and memory-mapping behavior for kernel debug tests, early
user-mode ABI work, and glibc startup probes.

## Implemented Calls

- `getpid`
- `read`
- `write`
- `open`
- `openat`
- `creat`
- `close`
- `close_range`
- `dup`, `dup2`, `dup3`
- `stat`
- `lstat`
- `fstat`
- `newfstatat`
- `lseek`
- `pread64`
- `pwrite64`
- `readv`
- `writev`
- `mkdir`
- `mkdirat`
- `unlink`
- `unlinkat`
- `chdir`
- `fchdir`
- `getcwd`
- `access`
- `faccessat`
- `faccessat2`
- `fcntl`
- `getdents64`
- `uname`
- `clock_gettime`
- `clock_getres`
- `nanosleep`
- `clock_nanosleep`
- `gettimeofday`
- `time`
- `getpid`
- `getppid`
- `gettid`
- `getuid`, `geteuid`, `getgid`, `getegid`
- `getrlimit`
- `prlimit64`
- `sysinfo`
- `brk`
- `mmap`
- `mprotect`
- `munmap`
- `arch_prctl`
- `futex`
- `getrandom`
- `set_tid_address`
- `set_robust_list`
- `rseq`
- `rt_sigaction`
- `rt_sigprocmask`
- `sched_yield`
- `exit`
- `exit_group`
- `umask`
- `ok_debug`

`stdin`, `stdout`, and `stderr` are reserved as descriptors `0`, `1`, and `2`.
File descriptors and memory mappings are fixed-capacity kernel objects, not
hosted runtime handles. Path operations resolve absolute paths, working-directory
relative paths, and `openat`-style directory-relative paths against the RAM VFS.
Process, signal, futex, and rseq calls currently use single-process fallback
semantics unless a richer subsystem exists behind them.

## Compatibility Goals

- POSIX process model: fork, exec, wait, signals, process groups, sessions.
- POSIX file model: file descriptors, directories, permissions, mounts, pipes.
- POSIX memory model: `mmap`, `mprotect`, `brk`, copy-on-write, user/kernel
  protection.
- POSIX time model: monotonic and realtime clocks, timers, sleeps.
- POSIX IPC: pipes, signals, shared memory, and message queues.
- POSIX device model: `ioctl`, TTY, block devices, network sockets.

## Current Contract

New syscall handlers should return `ok::syscall::Response`, must preserve
negative error semantics at the user boundary, and should map internal
`ok::StatusCode` values to stable POSIX `errno` values when the libc/user ABI
layer is added. The POSIX service itself should remain freestanding and should
use fixed-capacity kernel containers.
