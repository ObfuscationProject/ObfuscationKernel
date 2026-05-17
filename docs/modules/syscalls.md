# System Calls

`ok::syscall::Table` maps syscall numbers to handlers. Function-backed handlers
must satisfy `SyscallCallable`.

Implemented baseline syscalls:

- `getpid`
- `write`
- `ok_debug`

The number table reserves common POSIX/Linux-compatible syscall IDs to make the
future user ABI predictable.

