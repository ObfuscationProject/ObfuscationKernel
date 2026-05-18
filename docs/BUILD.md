# Build

The primary build system is xmake. The project uses xmake's built-in
architecture setting (`-a/--arch`) as the only architecture selector.

`okernel` is freestanding C++23 and automatically selects the matching
`ok-*-elf` toolchain for the configured xmake architecture. For example:

```sh
xmake f -c -m release -a x86_64
xmake toolchain-check
xmake -y -b okernel
```

The output follows xmake's normal layout:

```text
build/linux/x86_64/release/libokernel.a
```

## Supported Architectures

- `i386`
- `x86_64`
- `aarch64`
- `arm32`
- `rv64`
- `rv32`
- `loongarch64`

## Toolchains

Build a missing freestanding GCC/binutils toolchain with:

```sh
xmake toolchains -a rv64 -j 4
```

Check the current architecture toolchain:

```sh
xmake toolchain-check
```

Check all supported toolchains:

```sh
xmake toolchain-check --all
```

If a required toolchain is missing, the build fails with the command to run, for
example:

```text
missing freestanding toolchain for rv64 (riscv64-elf). Run: xmake toolchains -a rv64
```

The toolchain declarations live in `xmake/toolchains.lua`; the architecture to
toolchain selection lives in `xmake/arch.lua`.

## Tests

`qemu_smoke` is registered as an xmake test. It builds a direct-execution test
binary for the current xmake architecture profile and runs the kernel module
smoke suite:

```sh
xmake test
```

The convenience task does the same for the current architecture:

```sh
xmake qemu-test
```

To temporarily test another architecture profile:

```sh
xmake qemu-test -a aarch64
```

The visual task shows the current architecture test result in a QEMU window:

```sh
xmake qemu-window-test
```

Use `--no-launch` in headless environments to only generate the bootable status
image.

## Freestanding Flags

`okernel` is compiled with:

- `-ffreestanding`
- `-fno-exceptions`
- `-fno-stack-protector`
- `-fno-use-cxa-atexit`
- `-fno-threadsafe-statics`
- `-frtti`

Hosted containers such as `std::vector`, `std::string`, `std::map`, and
`std::unordered_map` are not used by the kernel library.
