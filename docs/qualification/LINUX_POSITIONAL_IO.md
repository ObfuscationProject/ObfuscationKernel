# Bounded Linux positional I/O

The explicitly marked static Linux personality admits pinned `pread64` and
`pwrite64` syscalls on all ten architecture profiles. They share the ordinary
open descriptions, filesystem objects and checked native user-copy primitives.
This is a bounded bootstrap slice, not complete off64, signals, libc cancellation,
concurrent VM, async I/O, SMP or general Linux compatibility.

The raw offset is signed64: one LP64 word, adjacent slots3/4 on i386/RV32,
and an aligned pair4/5 after unused slot3 on ARM32/MIPS O32/PPC. MIPS copies
exactly stack+16 and stack+20; target byte significance determines pair order.
Negative offsets reject with EINVAL before descriptor lookup. Invalid descriptors,
nonseekable pipes/consoles and access-mode errors precede data copying.

Offsets and admitted transfer endpoints cannot exceed the target native signed
word limit (INT32_MAX for32-bit profiles). Rejection is EOVERFLOW, with no high-half
truncation. Counts above target SSIZE_MAX reject EINVAL. Each call copies at most
4096bytes and may return short success; the complete bounded output span is
validated before a read. General Linux partial-copy behavior at page faults is
not supplied by this policy. Zero-length writes never extend the file.

The common `PosixService::pwrite_with_policy` uses static policy selection.
Native `pwrite` writes at its supplied position. Linux positional writes honor
O_APPEND using checked current EOF, including endpoint-overflow admission, as
the pinned Linux implementation does. Both leave the shared description position
unchanged on success and failure. They never seek and restore or alter flags.
The retained description continues to name the same node across dup/fork/unlink.

SDK/module ABI14 requires fresh producer/consumer artifacts; native UAPI0.4.0
is unchanged. Provisional SDK14 exports from different unintegrated candidates
are not interchangeable. `scripts/test_linux_positional.py --sanitize` is a
hosted model; disable LeakSanitizer explicitly where the sandbox's ptrace policy
prevents it (`ASAN_OPTIONS=detect_leaks=0`). ASan/UBSan remain active.

OS `scripts/test_musl_userland.py --require-positional` runs the cumulative public
musl probe twice and checks exact offline `/home/musl-positional` bytes `aXYZef!`
and e2fsck. The runner rejects missing, extra, truncated or altered execution
markers. Actual target execution requires the corresponding retained result;
compilation and hosted tests alone do not qualify a profile.
