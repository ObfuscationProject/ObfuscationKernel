# Linux static ELF bootstrap metadata

`include/ok/abi/linux_exec.hpp` supplies a freestanding, statically selected
`ok::abi::linux_user::ExecBootstrap<Profile>` for all ten Linux ABI profiles:
i386, x86_64, aarch64, arm32, rv64, rv32, loongarch64, mips, mips64 and ppc.
This is metadata validation and auxiliary-vector serialization. It does not
demonstrate ELF execution, TLS register setup or process isolation.

## Admission and ownership

`inspect(prefix, file_bytes)` accepts an explicitly supplied ELF header/program
table prefix of at most 64 KiB and an independent complete file length. The prefix
must include the entire program table. No segment payload need be in the prefix.
The caller must obtain both inputs from one stable file identity and retain that
identity through any subsequent segment copying.

The helper selects the expected ELF class, byte order and machine for the
profile. It requires ET_EXEC, exact ELF32/ELF64 program-header entry sizes, a
nonempty bounded program table and no extended PHNUM encoding. PT_INTERP,
PT_DYNAMIC and ET_DYN are unsupported. Existing `sched::ElfLoader::load_prefix`
validates the general load segments, their file extents, overlap and executable
entry coverage; the helper reuses that result and checks target address width.
The returned `ExecImage` contains the existing normalized `LoadedElf` and the
Linux-specific `ExecMetadata`. It owns no pointers into the input.

The helper requires the complete program-header table to have exactly one
readable, file-backed PT_LOAD mapping with target-word alignment. `metadata.program_headers` is that load's
virtual address plus the exact file displacement. A PT_PHDR is optional; if
present, its single record must match the table's file offset, loaded virtual
address and complete file/memory extent. Uncovered, unreadable, ambiguous,
duplicate and inconsistent metadata is rejected. An arbitrary stack copy or the
executable entry address is never substituted for a missing loaded table.

At most one PT_TLS is admitted. Its initialized `p_filesz` bytes must have an
exact readable PT_LOAD mapping matching both file and virtual displacement.
The helper checks complete-file bounds, `p_filesz <= p_memsz`, target address
arithmetic, power-of-two alignment and virtual/file alignment congruence. Zero
alignment is normalized to one. For a pure `.tbss` template, the zero-byte
initialization anchor must be within or at the end of the containing load's
file-backed extent. Per-thread zero fill beyond `p_filesz` need not lie in the
PT_LOAD memory extent: it describes allocation in each thread's TLS block.
The result reports file offset, loaded template address, initialized size,
per-thread memory size and alignment; it allocates no TLS block or thread pointer.

Processor metadata such as PT_RISCV_ATTRIBUTES or PT_MIPS_OPTIONS is not
misinterpreted as a loaded/TLS extent. Their file and memory sizes can differ in
ways that would be invalid for PT_LOAD. GNU_STACK policy, processor ABI flags,
instruction-set requirements, canonical user-address policy, actual page
permissions and all copying remain the native loader's responsibility. The
underlying `LoadedElf` uses native `uptr`/`usize`; a 32-bit host cannot use this
interface to normalize arbitrary 64-bit executables.

## Auxiliary vector

`encode_auxv(destination, metadata, parameters)` returns the number of bytes
written, including an AT_NULL pair. It writes words in the selected target width
and byte order. It validates values and capacity before copying its bounded
local staging buffer, leaving all output unchanged on failure and trailing bytes
unchanged on success.

The mandatory pairs are AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_UID,
AT_EUID, AT_GID, AT_EGID and AT_SECURE. `AuxvParameters` provides the actual page
size and credentials. AT_EXECFN and AT_RANDOM appear only when the caller
provides those optional user addresses. Maximum output is 13 pairs: 104 bytes on
32-bit profiles or 208 bytes on 64-bit profiles, including AT_NULL.

An optional address is never dereferenced or treated as a kernel pointer. The
caller must establish and retain a terminated executable-name string and, when
AT_RANDOM is present, 16 bytes from a qualified entropy source. The helper checks
nonzero target representability and the complete 16-byte random address range.
It never invents random bytes, claims entropy quality or substitutes predictable
placeholder bytes. Omitting AT_RANDOM does not establish a secure libc startup.

The caller must separately build and align argc/argv/envp, copy this auxiliary
vector into a mapped stack, set architecture entry/TLS registers and transfer
control. This bounded auxiliary set is not a claim of complete Linux process
startup compatibility or dynamic linking support.

## Validation

```sh
python3 scripts/test_linux_exec.py
ASAN_OPTIONS=detect_leaks=0 python3 scripts/test_linux_exec.py --sanitize
python3 scripts/test_linux_exec.py --cross
python3 scripts/test_linux_exec.py --elf ppc ../ObfuscationOS/build/musl-bootstrap/ppc-pilot/bootstrap.elf
```

Hosted model tests cover all ten profiles, exact endian/word serialization,
short and malformed prefixes, arithmetic bounds, PHDR ambiguity/coverage,
duplicate PT_PHDR/PT_TLS, inconsistent TLS displacement, initialized/zero-fill
TLS extents, ignored processor metadata, optional auxiliary entries and atomic
failure. Deterministic byte mutations exercise bounded parsing under ASan/UBSan.
LeakSanitizer is disabled in the shown sandbox command because this execution
environment uses ptrace; parser address and undefined-behavior checks remain
enabled. The standalone link discards unrelated process services and excludes
their vptr sanitizer instrumentation; RTTI remains enabled.

`--cross` compiles both API entry points with each explicit freestanding target
compiler into a temporary directory and verifies each object's ELF class,
endianness and machine. It does not alter xmake configuration or execute CPUs.
`--elf PROFILE PATH` audits one exact file as admitted metadata;
`--reject-elf PROFILE PATH` requires rejection and reports its reason. Neither
option discovers arbitrary artifacts or claims execution.

The initial ten musl 1.2.5 bootstrap artifacts inspected on 2026-09-20 all had one
readable PT_TLS with file size and memory size four, alignment four, and no
PT_PHDR. i386, x86_64, rv64, rv32, loongarch64 and ppc-pilot mapped their program
tables in a readable PT_LOAD. Initial aarch64, arm32, mips and mips64 artifacts
started their first PT_LOAD at file offset 0x1000 and left their program tables
unmapped; the helper correctly rejected those four. This describes the inspected
builds, not a permanent architecture limitation: link layout must provide a real
readable table mapping before native startup uses this contract. The probes were
marked `built-not-executed`; their metadata admission is a separate gate from
native syscall dispatch, TLS and libc execution tests.

This new helper has no build/runtime integration in this change. The integration
owner must add reached debug/native coverage and validate both standalone kernel
boot and explicit OS consumption of the matching architecture/mode Kernel SDK.

## Follow-up linked probe audit

The OS's subsequent `tests/musl/bootstrap.ld` explicitly maps ELF/program headers
in a readable FILEHDR/PHDRS load segment and supplies a non-executable GNU_STACK.
All ten `build/musl-bootstrap-v2/<arch>/bootstrap.elf` artifacts pass this helper's
exact PHDR/TLS admission, including the four initial fixtures rejected above.
Later v4 builds retain those mappings and explicitly mark ELFOSABI_LINUX, retaining
the linker's MIPS ABI version. Those artifacts are still built-not-executed.
