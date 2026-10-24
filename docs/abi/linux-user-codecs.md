# Linux userspace ABI codec foundation

Status: executable host codec models for all ten retained profiles; **no native
Linux syscall personality and no musl execution qualification**. These new headers
are independent of the native/common UAPI and the existing trusted-buffer
`LinuxSyscallAbi` model. No trap, dispatcher, process or runtime path consumes them
yet. Native boot behavior is unchanged.

`include/ok/abi/linux_user.hpp` provides `ok::abi::linux_user::Codec<Profile>`:

- `decode_timespec` / `encode_timespec` distinguish two-target-long legacy
  timespec from the 16-byte Linux `__kernel_timespec` time64 record. Conversion
  checks exact record size, signed seconds range and normalized nanoseconds.
  On 32-bit Linux syscall ABIs the high `tv_nsec` input word is padding ignored
  by Linux 6.13.4 `get_timespec64`; codecs preserve that behavior across both byte
  orders and write zero padding. The public musl `struct timespec` is a separate
  layout and must not be substituted by casting.
- `decode_call` reads up to six raw arguments from a caller-populated register
  snapshot. MIPS O32 arguments five/six come from already-copied stack bytes at
  offsets 16/20. It never reads an integer user address. The required stack span
  depends on the declared argument count, so a four-argument call needs no stack
  copy. A signature-specific caller must separately decide pointer handling,
  scalar signedness, mmap byte/page offset rules and argument reorderings.
- `decode_argument64` checks 32-bit word pairs and their required even-slot
  alignment on ARM EABI, MIPS O32 and PowerPC32. i386/RV32 pairs have no implicit
  alignment padding. A 64-bit profile consumes one word. The caller selects the
  slot; the codec does not guess syscall signatures.
- `encode_return` / `decode_return` handle the reserved negative errno range,
  MIPS `a3` plus positive architecture errno, and PowerPC `CR0.SO` plus positive
  errno. Encoding preserves unrelated PowerPC condition bits. Invalid returns,
  narrowing overflow and failed time conversions leave destination records
  unchanged. Success and error are separate fields rather than an ambiguous
  signed host integer.
- `syscall_number` and `errno_number` use compile-time profile selection. The
  generated table contains 32 selected bootstrap-related syscall identifiers and
  eleven errno constants. An absent musl source definition returns `nullopt`.
  A defined number does not imply that its handler exists, that it is reachable
  in the current Kernel, or that its associated structure is implemented.

## Register snapshot boundary

`RegisterFile::integer` uses DWARF integer numbering for i386/x86_64, and ISA
integer register numbering elsewhere. This choice is explicit in the generated
manifest. Existing native trap structures cannot be reinterpreted as this type.
A future trap adapter must copy the named registers into their documented slots,
canonicalize 32-bit words to their low 32 bits, and then invoke the codec. The
snapshot contract rejects upper-bit garbage rather than silently truncating a
host integer. Actual Linux syscall-number truncation/restart/ptrace behavior
belongs to that later trap/personality adapter.

`RegisterFile::condition` holds the PowerPC 32-bit CR. Only the retained `sc`
convention is covered, not a new `scv` or PPC64 profile. MIPS O32 indirect syscall
multiplexing and seven/eight-argument calls are outside this six-argument batch.
The caller owns user-buffer validation, copying and lifetime across blocking;
none of these helpers establishes process isolation or synchronization.

## Source generation and reproducibility

`docs/abi/linux-profiles.json` records the exact musl release archive digest,
source-file hashes, per-profile calling conventions, explicit absent operations,
Linux number crosschecks and remaining qualification gaps. The generator parses
integer aliases/addition/subtraction without evaluating arbitrary source text.
All present syscall numbers are crosschecked against Linux 6.13.4's selected
architecture tables or generic UAPI; ARM private `set_tls` uses the selected EABI
base. MIPS architecture number bases and errno differences remain explicit.

```sh
python3 scripts/generate_linux_abi.py \
  --musl-archive=/path/to/musl-1.2.5.tar.gz \
  --linux-source=/path/to/linux-6.13.4 --check
python3 scripts/test_linux_user_abi.py
```

Omit `--check` only when deliberately regenerating the checked-in header/manifest.
The archive must match the pinned official musl 1.2.5 bytes. The local Linux file
hashes are reproducible source evidence; authenticity of a complete upstream
Linux archive has not yet been established. Ordinary codec tests need neither
source tree and perform no downloads.

The tests instantiate all ten codecs in one host process. Literal byte/register
vectors verify little/big endian, signed time32 boundaries, time64 padding,
short-buffer faults, O32 stack slots, pair alignment, syscall numbers, MIPS errno,
PowerPC flags and failure atomicity. A separate parser suite rejects cyclic or
executable macro expressions, wrong archives and incorrect ABI table selection.
Host sanitizers cover memory/undefined behavior; the test runner supports
`--sanitize`. In a ptrace-managed environment, `ASAN_OPTIONS=detect_leaks=0` is
needed because LeakSanitizer cannot inspect that environment; these codecs own no
heap allocations. This remains model coverage, including big-endian profiles.

## Integration gates still open

A production Linux personality needs actual per-architecture trap adapters,
selected syscall signatures, safe user copies, independently defined flags,
complete errno mapping, stat/timeval/signal/TTY/IPC layouts, TLS/auxv/ELF startup
and real process/VM/clock semantics. It must preserve existing native-personality
acceptance while executing musl programs in a separate qualified lane. SDK export
and isolated OS artifact consumption remain required when these interfaces enter
that runtime boundary. This batch does not update the current native runtime,
SDK generation, xmake configuration or release capability declarations.
