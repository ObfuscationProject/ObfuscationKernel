# Bounded Linux bootstrap adapter

`ok/user/linux_bootstrap.hpp` is the bounded template adapter used for explicitly
marked static Linux-personality executables. The native runtime selects it per
image; it does not establish general Linux binary compatibility or complete
set_tid_address/thread semantics. SDK/native-extension ABI is 9; native UAPI is
0.4.0. Exact SDK source/artifact receipts distinguish qualified revisions.

The adapter accepts only explicit bootstrap signatures: scalar/vectored read and
write, openat, close, fsync/fdatasync, anonymous private mmap/mmap2 at offset zero, unmap/protection, exit,
monotonic clock_gettime variants, process identity/fork/exec/wait,
set_tid_address and the selected architecture's
TLS setup. The x86_64 route accepts ARCH_SET_FS; the i386 user_desc route validates
the one full-range descriptor shape used by musl and assigns the actual existing
GS GDT slot 7. MIPS explicitly selects the executed RDHWR-emulation route. Other
operations return ENOSYS. It never calls the trusted-buffer syscall model with
an unchecked user pointer.

File/VM calls delegate through a statically bound callback to the existing native
copy boundary. The per-architecture bridge decodes actual saved registers and
copies only the declared MIPS O32 stack argument words. Time structures use the
independent endian/width codec; native timespec layout is not reused for Linux
legacy time32. The bounded anonymous flags translate the MIPS MAP_ANON value.
Results preserve the Linux MIPS/PPC error convention and remap the differing MIPS errors this subset can produce, including pathname errors.

ELF OSABI 0 retains the native personality; OSABI 3 selects Linux, with ABI
version 1 for MIPS O32/N64 and 0 for the other retained profiles. Other OSABI
values fail admission. This is an explicit distribution marking contract;
unmarked third-party Linux binaries are not detected from names or syscall
numbers. Exec publishes the personality only after image admission and mapping
succeed; fork copies it with process resources. Pending native callbacks retain
the original Linux saved registers until completion, and all completion sites
select the matching return codec.

The stack supplies argc, argv, envp and checked PHDR/PHENT/PHNUM, page size,
entry, real/effective IDs, secure state and executable-name auxiliary entries.
The program-header table must be mapped and readable. TLS template metadata is
validated; musl performs its own static TLS allocation and installs the actual
architecture thread pointer. No entropy source exists yet, so AT_RANDOM is
omitted instead of advertising deterministic bytes as randomness. This bootstrap
does not provide hardened stack-canary initialization or set-ID execution.

The single-thread exit path clears registered child-tid memory before retiring
its address space, ignoring an unmapped destination. It does not perform a futex
wake or establish thread-join semantics. Signal delivery, threads/futexes, remaining paths/stat layouts, file mappings,
wall clock, general Linux flags and complete errno/signature coverage are still
separate gates. No stubs are reported as completed POSIX operations.

`tests/host/native_poll_tests.cpp` executes this adapter with actual Kernel/native
handlers and a hosted copy/clock backend. It covers checked close, x86 FS and i386
user_desc, MIPS time64 bytes/errno and unknown-operation rejection. The companion
`scripts/test_linux_bootstrap.py` compiles all ten specializations freestanding.
These tests do not execute target instructions or prove musl startup.

`scripts/test_linux_runtime.py --elf /explicit/x86_64/bootstrap.elf` additionally
tests the actual image loader with a hosted mapping backend: maximum argv/envp,
auxv pointers, failed-exec rollback, personality publication, result selection,
and clear-child-tid before retirement. `--sanitize` enables ASan/UBSan.

The separate OS `scripts/test_musl_userland.py` lane packages exact audited probe
and native-parent ELFs on a scratch root. A real guest parent runs the musl child
twice, verifies wait/exit status, then the shell executes another native pipeline.
Its per-run serial log and input hashes are execution evidence only for the
specified architecture/image. Current qualification lives in the OS workstation
progress record; no all-ten or physical-hardware claim follows from a single pilot.

## File admission and synchronization

Linux open flags are decoded per architecture by `ok/abi/linux_open.hpp` before
calling the shared `openat_limited` service. The service reserves both process
and global description capacity before creation/truncation; failure releases
the pin. This is a serialized boot-CPU guarantee, not concurrent namespace or
I/O-failure transaction atomicity. `O_APPEND` starts reads at offset zero and
redirects each write to current EOF. Linux 32-bit opens without `O_LARGEFILE`
reject regular files larger than INT32_MAX before truncation; native and LP64
opens retain their existing size policy. Paths/components remain bounded at
255/31 bytes. `O_NOCTTY` performs no action because controlling TTY acquisition
is not implemented. Legacy open, fcntl, stat and general device opens are not
qualified by these regular-file/directory checks.

`fsync` and `fdatasync` use the same conservative common node synchronization;
errors remain visible to the caller. The musl `--require-files` guest lane
requires actual file/sync markers, then checks exact stored bytes and e2fsck
with QEMU stopped. This is clean synchronization, not power-loss recovery.

## Process compatibility

The Linux adapter shares native fork, exec and wait mechanisms. Clone accepts
only the architecture's SIGCHLD value with a null child stack. It does not create
threads or share an address space. Native gettid returns the scheduler ThreadId;
Linux gettid and set_tid_address return the process PID for this admitted
single-leader profile. A future thread implementation must define a coherent
Linux task/group identity namespace.

Wait4 decodes pid_t and options as 32-bit scalars on LP64 while preserving full
pointer words. RV32 musl uses waitid because that architecture has no wait4
syscall. The bounded RV32 adapter supports P_ALL/P_PID with WEXITED and optional
WNOHANG, validates all 128 output bytes before reaping, and reacquires process
storage after compaction. Rusage, groups, WNOWAIT and other waitid profiles remain
unsupported. The completion scratch path requires the current serialized VM
with readable writable mappings; concurrent unmapping needs a different owner.

Fork preflights the common process/child capacities and returns EAGAIN before
retaining resources. Arithmetic and reference-count overflow retain their own
errors. Hosted `scripts/test_linux_process.py --sanitize` checks all ten ABI
adapters, including mismatched native PID/TID and an actual moved-parent reap.
`scripts/test_process_resources.py` preserves common ownership/capacity tests.
These models do not execute guest instructions.

The OS `tests/musl/process.c` probe extends startup/file/heap checks with identity,
private fork bytes, inherited shared file offset, self-exec, exit status,
ECHILD, exhaustion, reap and retry. Select `--require-process` in its native
runner or `--musl-process` in the workstation qualifier. Offline checks require
`/home/musl-process` to contain `parentchild!`. Only the exact integrated guest
receipts qualify the combined implementation; staged earlier SDKs remain separate.
