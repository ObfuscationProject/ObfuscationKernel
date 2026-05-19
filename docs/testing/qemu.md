# QEMU Testing

The QEMU tests boot the compiled `kernel.bin` directly. There is no test
`main.cpp`, hosted debug wrapper, GRUB image, or external bootloader. The image
contains a small kernel-owned first-stage boot sector plus the linked kernel
payload. Both debug and release builds enter the same `kernel_main`; debug mode
adds `OK_ENABLE_TEST_POINTS` and emits serial/display diagnostics for the Python
runner to validate.

## Current Architecture Test

Configure one architecture with xmake's built-in `-a` option:

```sh
xmake f -c -m debug -a x86_64
xmake test
```

`xmake qemu-test` is a convenience wrapper for the same current-architecture
test:

```sh
xmake qemu-test
```

Pass `-a` only for a temporary one-off test of another architecture:

```sh
xmake qemu-test -a i386
```

The current bootable QEMU system targets are `i386` and `x86_64`. Other
architecture profiles can build `okernel`, but they intentionally fail
`qemu-test` until that architecture has real boot assembly, a linker script, and
a QEMU launch profile.

## Visual Test

The windowed task is also current-architecture only:

```sh
xmake qemu-window-test
```

It builds and runs the same `kernel.bin` in QEMU. The visible text comes from
the kernel's own VGA display path, while the script captures serial diagnostics
and prints the test result only after the QEMU window is closed. In headless
environments:

```sh
xmake qemu-window-test --no-launch
```

## Pass Signal

The debug kernel must print `OK_MODE debug`, `OK_DEBUG boot=complete`,
`OK_DISPLAY_TEXT`, and `OK_TEST_PASS`. The Python runner also verifies that
debug test points ran and that filesystem, EXT4, user-mode, and display checks
reported success. Any non-zero exit code or missing marker is a failure.

## CI Coverage

GitHub Actions has one matrix job. For each architecture it:

1. Builds or restores the matching freestanding GCC/binutils toolchain.
2. Configures xmake with `-a <arch>`.
3. Builds `okernel`.
4. Runs `xmake qemu-test` for bootable system targets.
