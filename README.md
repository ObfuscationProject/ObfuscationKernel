# ObfuscationKernel

ObfuscationKernel is a C++23 multi-architecture kernel framework built with xmake.
The current tree is a working foundation: it compiles a reusable kernel library,
builds a bootable `kernel.bin` for all ten system targets, and runs debug
kernel tests that exercise interrupts, memory management, scheduling, IPC,
POSIX-oriented syscalls, drivers, VFS, EXT4 parsing, and user-mode transition
state. The foundation also includes SMP topology state, a VGA-backed display
path, PCIe/USB HID driver scaffolding, a kernel debug shell, and a read-only
EXT4 superblock/block reader. A fixed RAM disk, a QEMU virtio-blk test disk, and
SimpleFS provide the first block-backed writable filesystem path for early
disk-management tests, and the debug kernel now includes virtio-gpu/ramfb test
plumbing plus an IPv4/UDP/TCP loopback stack for network-debug bring-up.

This is not yet a complete production POSIX kernel. The implementation defines
the ABI, module boundaries, architecture profiles, build flow, and regression
test harness needed to grow into one without rewriting the project structure.
For downstream user-space experiments, the public 0.1.x C UAPI starts at
`include/ok/uapi/syscall.h`; kernel C++ headers remain implementation headers
unless mirrored there.

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

## Quick Start

```sh
xmake f -c -y -m debug -a x86_64
xmake -y -b okernel
xmake test
```

Expected result:

```text
OK_TEST_PASS arch=x86_64 ...
```

Enable debug test points for the current architecture:

```sh
xmake f -c -y -m debug -a x86_64
xmake qemu-test
```

## Cross Toolchains

Freestanding GCC/binutils and the matching QEMU system emulator are provided by
`systoolchain 0.1.0` from the project-owned `ObfuscationRepo`. The package is
built from source by default, and its installation validates the compiler,
binary utilities, and QEMU before the kernel build starts:

```sh
xmake f -c -y -a x86_64
xmake -y -b okernel
```

`okernel` and `okernel_image` select that package toolchain only for their
freestanding targets. QEMU launchers receive the emulator directly from the
resolved package; machine, CPU, device, disk, and display arguments remain
owned by this project.

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

For a visible QEMU window that displays the current bootable debug kernel output:

```sh
xmake qemu-window-test
```

In graphical mode the kernel remains interactive after the debug checks pass:
keyboard input is handled by the kernel input stack and routed through the debug
shell, GUI compositor, display path, and serial console. On `i386`, `x86_64`,
`aarch64`, `arm32`, `rv64`, `rv32`, and `loongarch64`, the window path uses QEMU
`ramfb` initialized by the guest through fw_cfg DMA. Before entering the shell,
the kernel draws a colored pixel marker in the framebuffer. The ramfb console
uses a 960x540 pixel surface with spaced bitmap glyph cells, a GUI-backed `oksh`
terminal surface, and a fixed mouse status line. On `aarch64`, `arm32`, `rv64`,
and `rv32`, QEMU virtio keyboard/mouse devices feed the shell and framebuffer
pointer through a minimal virtio-mmio input path. The script reports after the
QEMU window is closed.

The window task accepts every supported architecture. `mips`, `mips64`, and
`ppc` use QEMU Malta/ppce500 machines that do not expose fw_cfg/standalone
`ramfb`, so their graphical window is a serial VC while they still exercise the
kernel's generic memory framebuffer and GUI compositor state.

The test scripts do not contain a kernel `main`. Debug and release builds enter
through the same `kernel_main`; debug builds enable `OK_ENABLE_TEST_POINTS` and
emit `OK_*` diagnostics through serial and the kernel display driver. The
current bootable QEMU system targets are `i386`, `x86_64`, `aarch64`, `arm32`,
`rv64`, `rv32`, `loongarch64`, `mips`, `mips64`, and `ppc`.

## Architecture Implementations

Generic kernel modules call the compile-time `ok::arch::ArchOperations` alias.
`ArchitectureOperations<A, Instructions>` uses concepts and static dispatch;
each architecture supplies `NativeInstructions<A>` under `src/arch/<arch>/ops.cpp`.
Those files provide architecture-specific cycle counters, fences, interrupt
control and halt primitives. Profile traits describe syscall and user-transition
models; those descriptions do not implement hardware trap entry/return. Inline assembly is guarded
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
- [v0.1.0 Roadmap](docs/ROADMAP.md)
- [Userland Development](docs/USERLAND.md)
- [Architecture Overview](docs/ARCHITECTURE.md)
- [POSIX Roadmap](docs/POSIX.md)
- [Architecture Notes](docs/arch/)
- [Module Notes](docs/modules/)
- [Kernel Modules](docs/modules/modules.md)
- [GUI](docs/modules/gui.md)

## Kernel SDK and distribution boundary

Export a built architecture/mode for downstream ObfuscationOS:

```sh
xmake f -y -m debug -a x86_64
xmake -y -b kernel-sdk
```

`build/sdk/<arch>/<mode>` contains the core and boot archives, native/UAPI
headers, linker script, standalone image and a checksummed manifest. The OS
consumes this artifact and compiles its own external modules, desktop and driver
adapters; it does not embed or rebuild kernel sources. All ten kernel profiles
remain supported. See [Architecture](docs/ARCHITECTURE.md).

CPU primitives and scheduler policy selection now use templates/static dispatch.
Run `python3 scripts/test_static_dispatch.py` to validate all ten profile types
and two scheduling policies on the host. These are explicitly model tests;
hardware MMU isolation, user execution, true SMP/context switching and arbitrary
ELF module relocation remain separate implementation milestones.
