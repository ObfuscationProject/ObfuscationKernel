# Build

The primary build system is xmake. Every target is compiled as C++23 and RTTI is
kept enabled so kernel modules can use inheritance and `dynamic_cast` where it is
the clearest boundary.

`okernel` is a freestanding target. It is compiled with:

- `-ffreestanding`
- `-fno-exceptions`
- `-fno-stack-protector`
- `-fno-use-cxa-atexit`
- `-fno-threadsafe-statics`
- `-frtti`

Hosted containers such as `std::vector`, `std::string`, `std::map`, and
`std::unordered_map` are not used by the kernel library.

## Native Build

```sh
xmake f -c --arch_target=host
xmake -y -b qemu_smoke
xmake run qemu_smoke
```

## Architecture Profiles

`--arch_target` selects compile-time traits:

```sh
xmake f -c --arch_target=rv64
```

Supported values are `host`, `i386`, `x86_64`, `aarch64`, `arm32`, `rv64`,
`rv32`, and `loongarch64`.

To build and directly smoke-test every architecture profile with debug test
points enabled:

```sh
xmake arch-check -m debug
```

To run a single hosted smoke test from any current configuration, including a
current freestanding `ok-*-elf` toolchain configuration:

```sh
xmake qemu-test
xmake qemu-test -a x86_64
```

Where a Linux user-mode compiler and qemu-user are installed, run the smoke
binary under qemu-user:

```sh
xmake qemu-test -a aarch64 --user
```

Use release mode to confirm the same profiles compile without debug test point
code:

```sh
xmake arch-check -m release
```

## Freestanding Architecture Builds

After installing the `*-elf` toolchains, build every freestanding architecture:

```sh
xmake freestanding-check
```

For a partial local setup:

```sh
xmake freestanding-check --allow-missing
```

This task builds `okernel` with the matching `ok-*-elf` toolchain and performs a
relocatable whole-archive link to catch unresolved libc/libstdc++ runtime
symbols.

The separate graphical demo task runs the architecture smoke matrix and displays
the result in a QEMU window:

```sh
xmake qemu-window-test
```

## Cross Toolchains

The script `scripts/build-toolchain.sh` builds binutils and GCC under
`toolchains/<triple>`. The xmake toolchains are predeclared in
`xmake/toolchains.lua`:

| xmake toolchain | Prefix directory |
| --- | --- |
| `ok-i386-elf` | `toolchains/i386-elf` |
| `ok-x86_64-elf` | `toolchains/x86_64-elf` |
| `ok-aarch64-elf` | `toolchains/aarch64-elf` |
| `ok-arm32-elf` | `toolchains/arm-none-eabi` |
| `ok-rv64-elf` | `toolchains/riscv64-elf` |
| `ok-rv32-elf` | `toolchains/riscv32-elf` |
| `ok-loongarch64-elf` | `toolchains/loongarch64-elf` |

Example:

```sh
xmake toolchains -a rv64 -j 4
xmake f -c --arch_target=rv64 --toolchain=ok-rv64-elf
xmake -y -b okernel
```

The current cross toolchains are freestanding `*-elf` toolchains intended for
kernel code. QEMU user-mode tests need Linux user-mode cross toolchains or a
future freestanding semihosting payload.

## Linux User-Mode Test Toolchains

The repository also declares xmake toolchains for qemu-user smoke tests:

| xmake toolchain | Compiler prefix |
| --- | --- |
| `ok-i386-linux` | `i686-linux-gnu` |
| `ok-aarch64-linux` | `aarch64-linux-gnu` |
| `ok-arm32-linux` | `arm-linux-gnueabihf` |
| `ok-rv64-linux` | `riscv64-linux-gnu` |

These are meant for CI and local QEMU user-mode testing, not for freestanding
kernel image builds.
