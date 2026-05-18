# ObfuscationKernel

ObfuscationKernel is a C++23 multi-architecture kernel framework built with xmake.
The current tree is a working foundation: it compiles a reusable kernel library,
boots a simulated kernel instance, and runs smoke tests that exercise interrupts,
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
- `host` for native development and fast smoke tests

## Quick Start

```sh
xmake f -c --arch_target=host
xmake -y -b qemu_smoke
xmake run qemu_smoke
```

Expected result:

```text
OK_TEST_PASS arch=host processes=1 free_frames=16384 syscalls=3 debug_test_points=0
```

Run every architecture profile with debug-only test points:

```sh
xmake arch-check -m debug
```

Build freestanding kernel libraries with the installed `*-elf` toolchains:

```sh
xmake freestanding-check
```

Use `--allow-missing` while developing on a machine that only has some
toolchains installed.

## Cross Toolchains

Build GCC/binutils toolchains into `toolchains/`:

```sh
xmake toolchains -a x86_64 -j 4
```

Then configure xmake with the matching toolchain:

```sh
xmake f -c --arch_target=x86_64 --toolchain=ok-x86_64-elf
xmake -y -b okernel
```

## QEMU Smoke Tests

The smoke runner executes a test binary directly on the host for `host`, or via
qemu-user for non-host user-mode ELF test binaries:

```sh
xmake qemu-test
```

`xmake qemu-test` intentionally rebuilds the hosted smoke target with a hosted
compiler even if the current xmake configuration uses a freestanding `*-elf`
toolchain. Use `--user` to select qemu-user and Linux cross toolchains where
available:

```sh
xmake qemu-test -a aarch64 --user
```

For a visible QEMU window that displays the current architecture smoke matrix:

```sh
xmake qemu-window-test
```

The bootless test model is intentionally separate from future bootloader or
firmware integration. It validates kernel module behavior through a normal test
entry point first, which keeps architecture bring-up fast and debuggable.

## Architecture Implementations

Generic kernel modules call `ok::arch::ArchOperations`. Each supported
architecture has a concrete implementation under `src/arch/<arch>/ops.cpp`.
Those files provide architecture-specific interrupt, syscall, user-transition,
cycle-counter, fence, and control-operation behavior. Inline assembly is guarded
by compiler target macros so profile checks can still run on a normal host
compiler, while real cross builds compile the matching target assembly.

`okernel` is built as freestanding C++23 with exceptions disabled and RTTI kept
enabled. The kernel library avoids hosted containers and allocation-heavy APIs;
fixed-capacity kernel containers live in `include/ok/core/fixed.hpp`, and
minimal C/C++ ABI support lives in `src/core/runtime.cpp`.

## Documentation

- [Build](docs/BUILD.md)
- [Testing](docs/testing/qemu.md)
- [Development Standard](docs/DEVELOPMENT.md)
- [Architecture Overview](docs/ARCHITECTURE.md)
- [POSIX Roadmap](docs/POSIX.md)
- [Architecture Notes](docs/arch/)
- [Module Notes](docs/modules/)
