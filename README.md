# ObfuscationKernel

ObfuscationKernel is a C++23 multi-architecture kernel framework built with xmake.
The current tree is a working foundation: it compiles a reusable kernel library,
boots a simulated kernel instance, and runs debug kernel tests that exercise interrupts,
memory management, scheduling, IPC, syscalls, drivers, VFS, and user-mode
transition state. The foundation also includes SMP topology state, a simple
framebuffer display driver, and a read-only EXT4 superblock/block reader.

This is not yet a complete production POSIX kernel. The implementation defines
the ABI, module boundaries, architecture profiles, build flow, and regression
test harness needed to grow into one without rewriting the project structure.

## Supported Architecture Profiles

- `i386`
- `x86_64`
- `aarch64`
- `arm32`
- `rv64`
- `rv32`
- `loongarch64`
- `mips`
- `mips64`
- `ppc`
- `ppc64`

## Quick Start

```sh
xmake f -c -m debug -a x86_64
xmake toolchain-check
xmake -y -b okernel
xmake qemu-test
```

Expected result:

```text
100% tests passed
```

Enable debug test points for the current architecture:

```sh
xmake f -c -m debug -a x86_64
xmake qemu-test
```

Check every configured freestanding toolchain:

```sh
xmake toolchain-check --all
```

## Cross Toolchains

Build GCC/binutils toolchains into `toolchains/`:

```sh
xmake toolchains -a x86_64 -j 4
```

Then configure xmake with the matching architecture. `okernel` selects the
matching freestanding toolchain automatically:

```sh
xmake f -c -a x86_64
xmake -y -b okernel
```

## QEMU Tests

```sh
xmake qemu-test
```

`qemu-test` tests the current xmake architecture. Pass `-a` only when you want a
temporary one-off test for another architecture:

```sh
xmake qemu-test -a aarch64
```

For a visible QEMU window that displays the current architecture debug kernel
output:

```sh
xmake qemu-window-test
```

The bootless test model is intentionally separate from future bootloader or
firmware integration. The test binary only calls `ok_kernel_main`; the debug
kernel performs boot, module checks, and diagnostic output itself.

## Architecture Implementations

Generic kernel modules call `ok::arch::ArchOperations`. Each supported
architecture has a concrete implementation under `src/arch/<arch>/ops.cpp`.
Those files provide architecture-specific interrupt, syscall, user-transition,
cycle-counter, fence, and control-operation behavior. Inline assembly is guarded
by compiler target macros so profile checks can still run on a normal host
compiler, while real cross builds compile the matching target assembly.

`okernel` is built as freestanding C++23 with exceptions disabled and RTTI kept
enabled. The kernel library avoids hosted containers and allocation-heavy APIs;
fixed-capacity kernel containers live in `include/ok/core/fixed.hpp`.

## Documentation

- [Build](docs/BUILD.md)
- [Testing](docs/testing/qemu.md)
- [Development Standard](docs/DEVELOPMENT.md)
- [Architecture Overview](docs/ARCHITECTURE.md)
- [POSIX Roadmap](docs/POSIX.md)
- [Architecture Notes](docs/arch/)
- [Module Notes](docs/modules/)
