# SMP scaling WP0b: descriptions.cpp code move

Second package of the smp-scaling work. WP0b is a pure code move that gives
WP5 (files, pipes, AF_UNIX, tty under fine locks) its own unit. Nothing
changes in behaviour, in exported headers, in the module ABI (81) or in the
UAPI. Base: WP0a (a10aeb9).

## What moved

- `src/sched/descriptions.cpp` (new) holds the `SchedulerState` open file
  description code: `allocate_description`, `install_descriptor`,
  `retain_description`, `release_description`, `close_descriptor`,
  `release_resources`, `record_resource_failure`, `consume_resources`,
  `drain_resources`; the pipe code (`pipe_has_endpoint`, `pipe_readiness`,
  `create_pipe`, `create_unix_socketpair`, `read_pipe`, `write_pipe`,
  `write_pipe_fragment`); and the five fragments `unix_rights.inc`,
  `unix_path.inc`, `descriptor_reservation.inc`, `pty_pair.inc` and
  `terminal_open.inc`. The text is verbatim (scheduler.cpp 1059-1319 and
  1924-2093 at 5815814).
- These stay in `scheduler.cpp`: `drain_all_resources` (it walks the process
  table, and the shutdown drain belongs to WP6), the small accessors before line
  1059 (`open_description`, `position_lock`, `resources_owner`, the
  service setters), `open_description_count`, and the `console_input.inc` and
  `job_control.inc` overlays.
- `src/sched/resource_drain.hpp` (new, private, not exported) holds
  `count_resource_event` and `ResourceDrainAdmission`, the two file-local
  helpers that both units use. They keep internal linkage, so each unit
  compiles them as before.
- Build lists: the kernel build already globs `src/sched/*.cpp`. Every hosted
  runner and host xmake project that names `scheduler.cpp` now also names
  `descriptions.cpp`. The three runners that copy an explicit file list
  (`test_native_futex_origin.py`, `test_native_syscall_restart_publication.py`,
  `test_native_pipe_vector_publication.py`) also copy the private header.

## No-behaviour-change oracle

The oracle builds all ten debug `okernel_image` profiles before and after the
move in the same build directories. It then compares per-symbol disassembly of
the old `scheduler.o` with the new `scheduler.o` + `descriptions.o` pair. For
each function and data symbol it checks two things:

- normalized `objdump -d --no-show-raw-insn` text;
- raw bytes with relocated fields cleared, plus a relocation list in which
  every target is keyed by name or, for `.LC`, `CSWTCH` and section-relative
  targets, by the bytes it points at.

The results:

- **Every other kernel object** is byte-identical after
  `objcopy --strip-debug` on all ten profiles (95, or 96 on x86_64).
- **-O0** (the exact xmake command lines with `-O0` appended): every symbol
  is identical on all ten profiles, 2217-2275 symbols per profile. That
  covers the 106 moved functions, the 141 that stay, and every COMDAT copy in
  both units.
- **-O2 with the cross-function optimizations off** (`-fno-inline`, the
  `-fno-ipa-*` family, `-fno-section-anchors`, `-falign-*=1`): every symbol is
  identical on all ten profiles. On x86, 1 (i386) and 22 (x86_64) functions
  match only after folding alignment padding: GCC 14 pads tight loops to 64
  bytes counted from the section offset.
- **-O2 as built**: 58-145 symbols per profile differ. The causes are calls
  that now cross the unit boundary (for example `open_description` is no
  longer inlined into the PTY code), changed inlining budgets, IPA register
  allocation, section-anchor offsets (aarch64, ppc) and constant-pool
  sharing. The -O0 and no-IPA results show that each function's own code is
  unchanged.
- **Negative control**: flipping one comparison in `pipe_has_endpoint` is
  reported as exactly that one difference on x86_64, mips, arm32 and rv64.

The oracle scripts and per-profile results belong with the WP0b receipts in
ObfuscationEvidence.

## Validation

- `xmake test` at `-smp 1`: `OK_TEST_PASS` on all ten profiles (debug).
- `scripts/hosted_suite.py` with the toolchain root: 164 passed and 0 failed,
  the same runner set and statuses as on a10aeb9. 78 of those runners compile
  `scheduler.cpp`, either from an explicit list or through the `src/sched`
  glob. `tests/host/tcp_streams_xmake.lua` (hosted kind) passes with 1992
  checks on both trees.
- Not run, because they need inputs the suite does not make:
  - `native_futex_origin`, `native_pipe_vector_publication` and
    `native_syscall_restart_publication` need a PPC ELF;
  - `linux_chmod_service` needs a Linux oracle;
  - `ext4_mounted` needs the ext4 fixture;
  - `ext4_rename_relocation` needs a recorded attempt.

  `test_linux_stat.py --suite service --build-only` fails at link on both
  trees with the same six undefined symbols. Its shared source list lacks
  `memory.cpp`; that predates this package.
- The OS native distro boot on x86_64 (`qemu-distro --fs=simplefs
  --require-native`), against this tree's exported SDK, reaches
  `OK_USERLAND scope=hardware acceptance=pass`.
