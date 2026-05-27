# System Calls

`ok::syscall::Table` maps syscall numbers to handlers. Function-backed handlers
must satisfy `SyscallCallable`.

The public downstream header is `include/ok/uapi/syscall.h`. It is C-compatible
and freezes the 0.1.x syscall numbers, errno values, file flags, memory flags,
and simple native ABI structs used by the Linux-style dispatcher. Kernel C++
headers are still implementation details unless a symbol is mirrored through
that UAPI header.

`DispatchMode` records the intended syscall entry profile:

- `trap`: generic architecture trap path.
- `fast_path`: architecture fast syscall instruction path.
- `vdso_assisted`: user-mapped helper page path.

Implemented baseline syscall groups:

- FD I/O: `read`, `write`, `pread64`, `pwrite64`, `readv`, `writev`, `close`,
  `close_range`, `dup`, `dup2`, `dup3`, `fcntl`, `ioctl`.
- Paths and metadata: `open`, `openat`, `creat`, `stat`, `lstat`, `fstat`,
  `newfstatat`, `mkdir`, `mkdirat`, `unlink`, `unlinkat`, `chdir`, `fchdir`,
  `getcwd`, `access`, `faccessat`, `faccessat2`, `getdents64`.
- Time and identity: `getpid`, `getppid`, `gettid`, uid/gid queries,
  `clock_gettime`, `clock_getres`, `nanosleep`, `clock_nanosleep`,
  `gettimeofday`, `time`, `uname`, `sysinfo`.
- glibc startup support: `brk`, `mmap`, `mprotect`, `munmap`, `arch_prctl`,
  `futex`, `getrandom`, `getrlimit`, `prlimit64`, `set_tid_address`,
  `set_robust_list`, `rseq`, `rt_sigaction`, `rt_sigprocmask`, `sched_yield`,
  `exit`, `exit_group`, `umask`.
- Kernel debug: `ok_debug`.

The number table reserves common POSIX/Linux-compatible syscall IDs to make the
future user ABI predictable. File-oriented syscalls currently route into
`ok::posix::PosixService`, which is backed by the RAM VFS.

`LinuxSyscallAbi` currently implements the x86_64 register convention:
syscall number in `rax`, arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, and
`r9`, and the return value in `rax`. `LinuxSyscallDispatcher` maps failed
`Status` values to negative `OK_E*` errno values. Unknown syscalls and reserved
process/IPC syscalls that are not implemented in the current profile return
`-OK_ENOSYS`.
