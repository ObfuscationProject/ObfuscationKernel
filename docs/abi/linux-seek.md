# Bounded Linux seek adapter

`linux_bootstrap.hpp` admits Linux `lseek` and `_llseek` using the existing
POSIX seek operation and canonical open-description offset. The generated
numbers and manifest are pinned to Linux 6.13.4 `fs/read_write.c` and musl 1.2.5
`src/unistd/lseek.c`. Native UAPI remains unchanged. Added Operation entries
change the exported ProfileData layout, so the combined pipe/seek integration
requires SDK ABI 11. OS execution consumes an explicitly exported matching SDK.

| Profile | lseek | _llseek | Offset ABI |
|---|---:|---:|---|
| i386 | 19 | 140 | signed word / high-low halves |
| x86_64 | 8 | absent | signed word |
| aarch64 | 62 | absent | signed word |
| arm32 | 19 | 140 | signed word / high-low halves |
| rv64 | 62 | absent | signed word |
| rv32 | absent | 62 | high-low halves |
| loongarch64 | 62 | absent | signed word |
| mips O32 | 4019 | 4140 | signed word / high-low halves |
| mips64 N64 | 5008 | absent | signed word |
| ppc32 | 19 | 140 | signed word / high-low halves |

fd and whence are unsigned 32-bit scalar syscall arguments, even on LP64.
`lseek` sign-extends its native-word offset; `_llseek` joins the unsigned high
then low 32-bit halves into one signed 64-bit offset. MIPS O32 takes the fifth
whence argument from checked user stack memory at SP+16, without reading the
unused sixth word or applying an aligned off64-pair rule. `_llseek` writes
exactly eight target-endian result bytes and returns zero; `lseek` returns the
resulting native-word position. MIPS/PPC retain their own return/error registers.

Descriptor validation precedes whence admission. Values above Linux SEEK_MAX=4
return EINVAL; unseekable pipe/console endpoints return ESPIPE. Seekable nodes
admit SET/CUR/END only; DATA/HOLE return EINVAL until extent semantics are
implemented. Negative resulting positions return EINVAL, arithmetic/native
capacity overflow returns EOVERFLOW, and failed seeks preserve the offset.
Current native capacity remains INT32_MAX on 32-bit kernels, INT64_MAX on 64-bit
kernels. `_llseek` does not upgrade the 32-bit kernel's file-position capacity.
This differs from a Linux filesystem capable of larger positions. Raw Linux
lseek narrows the successful result after its filesystem seek; the codec also
performs that result check, while the current 32-bit common service rejects
out-of-capacity positions before mutation. No full 32-bit off64 claim is made.

The result pointer is deliberately not preflighted: Linux `_llseek` changes the
shared position before `copy_to_user`. A copy fault returns EFAULT and retains
the new offset, including through duplicated/fork-inherited descriptors. A
partial copy may alter some output bytes. A failed seek never attempts output.
The adapter does not introduce SMP file-position synchronization.

Run `scripts/test_linux_seek.py --sanitize` for all-ten hosted adapter/service
models. These use one LP64 host service: the codecs model 32-bit arguments and
results, but hosted common storage capacity remains the host capacity. Actual
32-bit capacity is checked in the target musl `tests/musl/seek.c` probes.
`test_linux_bootstrap.py` compiles the adapter for all ten targets.
The OS `test_musl_userland.py --require-seek` and `qualify_workstation.py
--musl-seek` require two complete seek receipts, preceding descriptor/path/
process/file/heap gates, native-parent exit checks and offline persisted bytes.
Tests include shared/fork offsets, negative/overflow ordering, unaligned and
faulting `_llseek` output, and failed-copy position retention on actual 32-bit
MMU mappings. Hosted models, QEMU execution and physical hardware remain
separate evidence categories.
