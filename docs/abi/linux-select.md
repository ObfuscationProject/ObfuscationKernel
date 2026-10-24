# Bounded Linux select and null-mask pselect

The Linux personality admits select, _newselect, pselect6 and pselect6_time64
only on their actual syscall profiles. All ten architecture profiles remain
available. The generator pins Linux 6.13.4 and musl 1.2.5 source identities;
source inspection and hosted models are distinct from QEMU execution.

| Profile | direct select | old pointer select | pselect6 | pselect6_time64 |
| --- | --- | --- | --- | --- |
| i386 | 142 | 82 | 308 | 413 |
| x86_64 | 23 | absent | 270 | absent |
| arm32 EABI | 142 | absent | 335 | 413 |
| aarch64 | absent | absent | 72 | absent |
| rv32 | absent | absent | absent | 413 |
| rv64 | absent | absent | 72 | absent |
| loongarch64 | absent | absent | 72 | absent |
| mips O32 | 4142 | absent | 4301 | 4413 |
| mips64 N64 | 5022 | absent | 5260 | absent |
| ppc32 | 142 | 82 | 280 | 413 |

Old select reads a five-long argument record. MIPS O32 pselect copies arguments
five and six from stack+16 and stack+20. Bitsets use the target long width and
endianness; BE64 fd0 is byte7, and even nfds1 copies a full eight-byte word.
The signed low32 nfds is rejected if negative and otherwise clamped to the
current allocated descriptor-table capacity32. This follows Linux's allocation
clamp without claiming the host's larger fdtable or libc FD_SETSIZE. Static
assertions prevent silently retaining the32-bit mask after table growth.

All input sets are copied before output. Tail bits are ignored and zeroed on
output; bytes beyond the effective rounded word are untouched. A selected closed
FD produces EBADF before any provider is scanned, including exception-only
membership. Read readiness is IN/HUP/ERR; write readiness is OUT/ERR. Current
providers have no priority source, so HUP/ERR never imply exceptional data.
The return counts bits across sets: one regular FD selected for read and write
returns2. Regular-file readiness does not grant access permission.

The typed SelectRequest reuses the serialized boot-CPU poll wait engine. Input
sets, output addresses and timeout metadata are frozen on admission; each
selected FD retains its description once, even if selected in multiple sets.
Completion checks current output mappings, writes read/write/except in order,
and stops on the first set-copy fault. Earlier writes remain visible. Timeout
copyback follows the sets and its failure never replaces the syscall result.
Readonly destinations may block and fail only at completion. Saved-frame
completion occurs once; no syscall replay or input reread occurs. Native exec
success, exit and teardown cancel registrations before releasing native memory.

A nonnull pselect argument container is copied before timeout input. Null inner
signal pointers ignore the supplied size; nonnull pointers require the target's
signal size (MIPS16, others8), a readable mask, then return ENOSYS. No signal mask
is installed, including an all-zero one. The unsupported-mask path does not copy
time remaining or register a wait.

Raw select normalizes signed timeval microseconds using target-long arithmetic
before validating the interval. Unsigned addition explicitly models target-word
wrap without C++ overflow. Normalized zero completes without timeout copyback.
Positive intervals retain a full end-time separate from the saturated scheduler
deadline; timeval remainder is floored to microseconds. pselect uses legacy or
time64 timespecs and32-bit time64 nanosecond padding rules. Errors after core
entry (nfds, masks, descriptors, copyout) still attempt remaining-time output.
Public musl select/pselect preserve the application's timeout through their
local raw records.

Exported Operation entries and readiness request metadata require Kernel SDK
and module ABI14; native UAPI0.4.0 is unchanged. OS admission rejects ABI13 and
uses only explicit architecture/mode SDK artifacts.

Hosted tests cover all ten codecs and raw adapters, independent bitmap bytes,
validation and alias ordering, reference rollback, capacity, FD reuse, timeouts,
saved-frame completion and mapping revocation. The cumulative musl probe adds
public calls, legacy/time64 raw layouts, delayed fork/pipe wake and exception
controls. Acceptance requires two complete select receipts and every previous
musl gate. No socket, signal interruption/restart, cancellation, SMP wait queue
or physical hardware behavior is claimed.
