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
model.

## Boot Flow

For `i386` and `x86_64`, the system boot path is:

1. The raw `kernel.bin` boot sector is executed by BIOS/QEMU at `0x7c00`.
2. The boot sector loads the kernel payload into memory and enters protected
   mode.
3. `src/arch/i386/boot.S` clears normal `.bss`, sets up the bootstrap stack,
   and calls `kernel_main`; `src/arch/x86_64/boot.S` additionally builds early
   identity page tables and enables PAE plus long mode before calling
   `kernel_main`.
4. `kernel_main` initializes serial/VGA output, selects debug or normal mode,
   and calls the architecture-independent kernel entry.

`ok::Kernel::boot` then performs the generic boot sequence:

1. Select architecture operations from xmake's configured architecture.
2. Initialize physical memory from a memory map.
3. Register and start built-in drivers.
4. Register timer interrupt and baseline syscalls.
5. Create and schedule the idle process.
6. Create `/tmp/kernel.log` in the RAM VFS.

The debug test suite then validates one operation from every core module.

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
| `mips` | `src/arch/mips/ops.cpp` |
| `mips64` | `src/arch/mips64/ops.cpp` |
| `ppc` | `src/arch/ppc/ops.cpp` |
| `ppc64` | `src/arch/ppc64/ops.cpp` |

Inline assembly is used for low-level primitives such as cycle counters, memory
fences, interrupt control, halt, and early boot entry. The freestanding library
profile keeps privileged instructions guarded where they must still compile
under non-system test builds; the bootable kernel target uses the real
architecture entry files for implemented system targets.
