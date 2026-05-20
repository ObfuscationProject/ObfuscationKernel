# Build

The primary build system is xmake. The project uses xmake's built-in
architecture setting (`-a/--arch`) as the only architecture selector.

`okernel` is freestanding C++23 and automatically selects the matching
`ok-*` cross toolchain for the configured xmake architecture. For example:

```sh
xmake f -c -m debug -a x86_64
xmake toolchain-check
xmake -y -b okernel
```

The output follows xmake's normal layout:

```text
build/linux/x86_64/debug/libokernel.a
```

The boot image target is named `okernel_image`. It depends on `okernel`, then
adds only `kernel_main`, platform I/O, boot entry assembly, and image packaging.
For implemented image targets it produces:

```text
build/linux/<arch>/<mode>/kernel.elf
build/linux/<arch>/<mode>/kernel_payload.bin
build/linux/<arch>/<mode>/kernel.bin
```

For BIOS x86 targets, `kernel.bin` is a single raw disk image containing the
first-stage kernel boot sector and the linked kernel payload. It does not use
GRUB or an external bootloader. AArch64 builds a Linux `Image`-style raw payload
with the 64-byte ARM64 header. RV64 keeps `kernel.bin` as the linked ELF image
because QEMU virt can jump directly to that ELF entry with `-kernel`.

## Supported Architectures

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

`ppc64` uses GCC's supported big-endian `powerpc64-linux-gnu` target behind the
local `ok-ppc64-elf` wrapper because upstream GCC does not ship a
`powerpc64-elf` configuration.

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

`xmake test` always validates the current architecture's freestanding `okernel`
profile. For bootable system targets (`i386`, `x86_64`, `aarch64`, and `rv64`)
it also builds `okernel_image`, boots the generated `kernel.bin` in QEMU, and
parses the debug kernel's `OK_*` diagnostic lines:

```sh
xmake test
```

The convenience task builds `okernel_image` and runs the Python checker for the
current architecture:

```sh
xmake qemu-test
```

To temporarily test another bootable architecture profile:

```sh
xmake qemu-test -a i386
```

The visual task shows the kernel's framebuffer text in a QEMU window:

```sh
xmake qemu-window-test
```

Use `--no-launch` in headless environments to boot the same kernel image without
launching a window and print the result through the same validation path.

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
