# First x86-64 shared-musl startup candidate

This private SDK34 candidate admits an x86-64 Linux `ET_DYN` main executable
whose `PT_INTERP` names `/lib/ld-musl-x86_64.so.1`. The ordinary static
`ET_EXEC` admission and auxv encoding remain in the ten-profile
`ExecBootstrap` path. No other architecture gains dynamic ELF admission in this
candidate.

The loader validates both ELF files independently, rejects nested
interpreters, non-readable metadata, overlapping mapped pages, writable and
executable loads, invalid program ranges, and a `PT_TLS` memory extent outside
its load mapping. The retained main and interpreter nodes populate unpublished
pages before either executable is released. The main program is relocated into
the `0x42000000` window; the interpreter is relocated into the `0x60000000`
window. The initial PC points at the interpreter entry. `AT_PHDR` and
`AT_ENTRY` describe the relocated main image, while `AT_BASE` identifies the
interpreter. `brk` starts after the main image rather than after libc.

The first guest uses a mode-matched musl 1.2.5 `libc.so` and a PIE with a real
`R_X86_64_RELATIVE` data pointer relocation, libc calls, TLS `errno`, and auxv
checks. A native parent reaps each dynamically linked child. A missing-loader
control verifies that failed `execve` leaves the old native child able to
report the error and that subsequent native commands still run. Hosted ELF
admission tests are models; QEMU serial records establish the narrower actual
user-mode execution claim. The emulator is terminated after the checks, so
offline fsck is a clean-run check, not a shutdown or power-loss durability
test.

`AT_RANDOM` is absent. The existing `getrandom` implementation derives bytes
from monotonic ticks and xorshift and is **functional-only**; it is not a
cryptographically secure entropy source. Successful libc startup and TLS
`errno` do not qualify secure process startup, TLS transport, `dlopen`, general
shared-object loading, pthreads, cancellation, or M5. The separate static
auxv model runs across all ten profiles, but this document makes no dynamic
execution claim for nine of them. The fixed address windows and eager copying
are a bounded bootstrap design pending general ASLR, shared file mappings,
page-cache coherence, and a security-qualified entropy subsystem.
