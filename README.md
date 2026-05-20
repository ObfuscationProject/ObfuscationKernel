# ObfuscationKernel

ObfuscationKernel is a C++23 multi-architecture kernel framework built with xmake.
The current tree is a working foundation: it compiles a reusable kernel library,
builds a bootable `kernel.bin` for the implemented system target, and runs debug
kernel tests that exercise interrupts, memory management, scheduling, IPC,
POSIX-oriented syscalls, drivers, VFS, EXT4 parsing, and user-mode transition
state. The foundation also includes SMP topology state, a VGA-backed display
path, PCIe/USB HID driver scaffolding, a kernel debug shell, and a read-only
EXT4 superblock/block reader.

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
xmake test
```

Expected result:

```text
OK_TEST_PASS arch=x86_64 ...
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

`xmake test` always validates the current architecture freestanding profile. For
bootable system targets it also builds the debug `okernel_image` target, places
the generated `kernel.bin` in QEMU, and validates the kernel's own serial debug
output. `qemu-test` is the direct wrapper for that booted check. Pass `-a` only
when you want a temporary one-off test for another architecture with a bootable
system target:

```sh
xmake qemu-test -a i386
```

For a visible QEMU window that displays the current x86 debug kernel output:

```sh
xmake qemu-window-test
```

In graphical x86 mode the kernel remains interactive after the debug checks pass:
keyboard input is handled by the kernel input stack and routed through the
debug shell, display path, and serial console. The script reports after the QEMU
window is closed.

The test scripts do not contain a kernel `main`. Debug and release builds enter
through the same `kernel_main`; debug builds enable `OK_ENABLE_TEST_POINTS` and
emit `OK_*` diagnostics through serial and the kernel display driver. The
current bootable QEMU system targets are `i386`, `x86_64`, `aarch64`, and
`rv64`. Other architecture profiles build the freestanding kernel library and
have architecture-specific operation implementations, with documented Linux boot
contracts until their platform I/O, linker scripts, and QEMU launch profiles are
ready to be promoted to `qemu-test`.

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
- [Boot Profiles](docs/BOOT.md)
- [Testing](docs/testing/qemu.md)
- [Development Standard](docs/DEVELOPMENT.md)
- [Architecture Overview](docs/ARCHITECTURE.md)
- [POSIX Roadmap](docs/POSIX.md)
- [Architecture Notes](docs/arch/)
- [Module Notes](docs/modules/)
