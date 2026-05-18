# Architecture Overview

The kernel is split into narrow C++23 modules with virtual interfaces at hardware
or policy boundaries and concepts at compile-time extension points.

## Layers

- `include/ok/arch`: CPU profile traits and architecture operation interface.
- `src/arch/<arch>`: concrete architecture operation implementations and
  guarded inline assembly.
- `include/ok/interrupt`: interrupt handler registration and dispatch.
- `include/ok/memory`: physical frame allocation and address-space interface.
- `include/ok/sched`: process/thread model and scheduler policy.
- `include/ok/ipc`: bounded message channels.
- `include/ok/syscall`: POSIX-oriented syscall table and handlers.
- `include/ok/driver`: driver base class and basic console/timer/block drivers.
- `include/ok/fs`: RAM-backed VFS node model.
- `include/ok/user`: user-mode transition gateway.
- `include/ok/core`: kernel composition and shared types.

## C++ Design Rules

Inheritance is used for runtime polymorphism where implementations are expected
to differ by machine, bus, device, or policy. Concepts are used for local,
zero-overhead extension points such as function-backed interrupt handlers,
syscall handlers, serializable IPC payloads, and driver registration.

RTTI is intentionally enabled so type metadata is available to kernel code.
Current freestanding code avoids `dynamic_cast` in core paths to keep the ABI
surface explicit and self-contained.

The kernel target is freestanding. Dynamic allocation and hosted containers are
kept out of `okernel`; fixed-capacity containers provide the current storage
model. `src/core/runtime.cpp` supplies minimal memory and C++ ABI symbols so the
kernel does not depend on libc, libstdc++, or libsupc++ at link time.

## Boot Flow

`ok::Kernel::boot` performs the current simulated boot sequence:

1. Select architecture operations from xmake's configured architecture.
2. Initialize physical memory from a memory map.
3. Register and start built-in drivers.
4. Register timer interrupt and baseline syscalls.
5. Create and schedule the idle process.
6. Create `/tmp/kernel.log` in the RAM VFS.

The smoke suite then validates one operation from every core module.

## Architecture-Specific Operations

The architecture-independent kernel code depends on `ok::arch::ArchOperations`.
The concrete implementations are separate files:

| Profile | Implementation |
| --- | --- |
| `i386` | `src/arch/i386/ops.cpp` |
| `x86_64` | `src/arch/x86_64/ops.cpp` |
| `aarch64` | `src/arch/aarch64/ops.cpp` |
| `arm32` | `src/arch/arm32/ops.cpp` |
| `rv64` | `src/arch/rv64/ops.cpp` |
| `rv32` | `src/arch/rv32/ops.cpp` |
| `loongarch64` | `src/arch/loongarch64/ops.cpp` |

Inline assembly is used for safe low-level primitives such as cycle counters and
memory fences. Privileged instructions such as interrupt enable/disable and CPU
idle are guarded behind `OK_USE_PRIVILEGED_ASM`, because the current test harness
executes as a normal user-mode process.
