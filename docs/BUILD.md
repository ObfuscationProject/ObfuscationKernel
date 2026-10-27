# Build

The primary build system is xmake. The project uses xmake's built-in
architecture setting (`-a/--arch`) as the only architecture selector.

`okernel` is freestanding C++23 and automatically selects the matching
`systoolchain` package for the configured xmake architecture. For example:

```sh
xmake f -c -y -m debug -a x86_64
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

## Toolchains

Build and resolve the freestanding GCC/binutils/QEMU package for an
architecture with:

```sh
xmake f -c -y -a rv64
```

The package downloads the SysToolchain release archive for the target and
verifies its `.sha256`; `build_from_source = true` is the fallback for hosts
without a published archive. The packaged QEMU needs the host GLib, Pixman and
SDL2 shared libraries. Package installation validates the compiler, binary
utilities, and matching QEMU executable. Architecture-to-
triple and QEMU launch-profile selections remain in `xmake/arch.lua`; xmake
resolves the installed tools from its package cache.

## Tests

`xmake test` always validates the current architecture's freestanding `okernel`
profile. It also builds `okernel_image`, boots the generated `kernel.bin` in
QEMU, and parses the debug kernel's `OK_*` diagnostic lines:

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
xmake qemu-window-test -a rv64 --no-launch
```

Use `--no-launch` in headless environments to boot the same kernel image without
launching a window and print the result through the same validation path. The
task accepts every configured architecture.

Normal release builds do not enter the kernel GUI desktop by default. Enable the
independent GUI build option when you want a non-debug graphical session:

```sh
xmake f -c -y -m release -a x86_64 --kernel_gui=y
xmake -y -b okernel_image
```

The kernel rescue applications in `src/rescue/` (debug shell, file manager, task
manager) are compiled only with `kernel_gui` or in debug builds, whose test points
exercise them. Other release builds link `src/rescue/disabled.cpp`, which keeps the
same interfaces and behaves as if the applications were never opened, so the
`Kernel` layout and the SDK symbols do not depend on the option.

The convenience launcher builds with that option and opens a QEMU window:

```sh
xmake qemu-gui
```

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
