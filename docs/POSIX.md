# POSIX Roadmap

The syscall namespace uses POSIX/Linux-compatible numbers for common calls such
as `read`, `write`, `open`, `close`, `getpid`, `fork`, `execve`, and `exit`.
Only `getpid`, `write`, and `ok_debug` are implemented in the current baseline.

## Compatibility Goals

- POSIX process model: fork, exec, wait, signals, process groups, sessions.
- POSIX file model: file descriptors, directories, permissions, mounts, pipes.
- POSIX memory model: `mmap`, `mprotect`, `brk`, copy-on-write, user/kernel
  protection.
- POSIX time model: monotonic and realtime clocks, timers, sleeps.
- POSIX IPC: pipes, signals, shared memory, and message queues.
- POSIX device model: `ioctl`, TTY, block devices, network sockets.

## Current Contract

The baseline keeps POSIX as an ABI direction, not a completed claim. New syscall
handlers should return `ok::syscall::Response`, must preserve negative error
semantics at the user boundary, and should map internal `ok::StatusCode` values
to stable POSIX `errno` values when the libc/user ABI layer is added.

