# Native user runtime layout

The native runtime is one serialized boot-CPU service compiled as separate
translation units with one responsibility each. `runtime/private.hpp` is the only
header the units share; it declares exactly what crosses a unit boundary.

| Unit | Responsibility |
| --- | --- |
| `runtime.cpp` | Service state (entry-stack pool, syscall origins, VFS continuations, failure and shutdown state), diagnostics output, native thread preparation and retirement, syscall result encoding, process completion, shutdown, termination and release |
| `runtime/loader.cpp` | ELF admission, the initial stack (argv/envp/auxv), image installation, `/init` installation and `execve` |
| `runtime/vm.cpp` | Native page pool and backend table storage, address-space quarantine and retirement, mmap/mprotect/munmap/brk, fork page sharing and copies |
| `runtime/wait.cpp` | poll/select readiness waits and wait4 child waits |
| `runtime/futex.cpp` | futex wait/wake/requeue/wake-op, shared futex keys, robust lists and `clear_child_tid` |
| `runtime/pipe.cpp` | Blocking pipe read/write continuations, including vectored I/O |
| `runtime/pty.cpp` | Raw PTY pair I/O continuations and their owners |
| `runtime/net.cpp` | Native and Linux UDP receive owners, TCP immediate operations, native UDP socket calls |
| `runtime/signals.cpp` | Private signal delivery, frames and waits, signal interruption of blocking calls, per-architecture signal state |
| `runtime/entry.cpp` | `start`/`step`, user entry through entry-stack leases, syscall dispatch |

Self-contained helpers that hosted tests also include stay as headers beside the
units: `native_syscall_origin.hpp`, `native_shared_capture.hpp`,
`native_syscall_restart_publication.hpp`, `native_poll_mask.hpp`,
`linux_udp_*.hpp`, `tcp_*.hpp` and `signals/*.hpp`.

Architecture signal frames, delivery, sigreturn decoding and the private
readiness state live in one `include/ok/arch/<arch>/signal.hpp` per
architecture (`i386`, `x86_64`, `aarch64`, `arm32`, `riscv` for rv32 and rv64,
`loongarch64`, `mips`, `mips64`, `ppc`); `runtime/private.hpp` selects the
configured one.

Rules:

- A helper used by one unit stays in that unit's anonymous namespace; only
  cross-unit entities are declared in `runtime/private.hpp`. Public entry points
  stay declared in `include/ok/user/`, `native_lifecycle.hpp` and
  `signals/private_signal_scheduler_relocation.hpp`.
- All runtime state is constant-initialized. The kernel never runs static
  constructors, so no runtime object may carry an `.init_array` entry.
- Hosted tests reach private state by compiling every unit as one translation
  unit through `tests/host/native_runtime_units.hpp`; runner scripts exclude
  `src/user/runtime.cpp` and `src/user/runtime/*.cpp` from separate compilation.
- New code goes into the unit that owns its responsibility. Headers under
  `src/user/` are self-contained (`#pragma once`); textual fragments included in
  the middle of a namespace are not reintroduced.
  `scripts/check_runtime_split.py` enforces both and keeps `runtime.cpp` from
  regrowing.
