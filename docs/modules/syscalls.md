# System Calls

`ok::syscall::Table` maps syscall numbers to handlers. Function-backed handlers
must satisfy `SyscallCallable`.

`DispatchMode` records the intended syscall entry profile:

- `trap`: generic architecture trap path.
- `fast_path`: architecture fast syscall instruction path.
- `vdso_assisted`: user-mapped helper page path.

Implemented baseline syscalls:

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

The number table reserves common POSIX/Linux-compatible syscall IDs to make the
future user ABI predictable. File-oriented syscalls currently route into
`ok::posix::PosixService`, which is backed by the RAM VFS.
