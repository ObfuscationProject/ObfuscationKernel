# System Calls

`ok::syscall::Table` maps syscall numbers to handlers. Function-backed handlers
must satisfy `SyscallCallable`.

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
