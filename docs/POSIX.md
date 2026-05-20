# POSIX Interface

The syscall namespace uses POSIX/Linux-compatible numbers for common calls such
as `read`, `write`, `open`, `close`, `getpid`, `fork`, `execve`, and `exit`.

The current kernel exposes a bounded POSIX service over the RAM VFS. It is not a
complete POSIX implementation yet, but it provides enough real file-descriptor
behavior for kernel debug tests and early user-mode ABI work.

## Implemented Calls

- `getpid`
- `read`
- `write`
- `open`
- `close`
- `stat`
- `mkdir`
- `unlink`
- `chdir`
- `getcwd`
- `uname`
- `clock_gettime`
- `ok_debug`

`stdin`, `stdout`, and `stderr` are reserved as descriptors `0`, `1`, and `2`.
File descriptors are fixed-capacity kernel objects, not hosted runtime handles.
Path operations currently resolve absolute paths and simple working-directory
relative paths against the RAM VFS.

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
