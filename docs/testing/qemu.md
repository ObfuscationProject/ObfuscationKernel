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

The current bootable QEMU system test targets are `i386`, `x86_64`, `aarch64`,
and `rv64`. Other architecture profiles can build `okernel` and intentionally
fail `qemu-test` until that architecture has a complete early platform contract
and a validated QEMU launch profile.

`xmake test` always has a freestanding profile test for `okernel`. On
bootable system targets it also runs the `okernel_image` QEMU boot test.

Every booted QEMU check also creates a temporary 16 MiB disk image and attaches
it as `virtio-blk-pci`. The kernel binds that PCI device as `virtio-blk0`, then
formats and exercises SimpleFS/EXT4 through the generic block-device interface.

## Visual Test

The windowed task is also current-architecture only:

```sh
xmake qemu-window-test
```

It builds and runs the same `kernel.bin` in QEMU. The visible text comes from
the kernel's own display path: x86/i386 use the virtio-gpu PCI path, while
`aarch64` and `rv64` initialize QEMU ramfb through fw_cfg DMA and draw pixels
directly into guest RAM.
The script captures serial diagnostics and prints the test result only after
the QEMU window is closed. Use the headless validation form in environments
without a graphical display:

```sh
xmake qemu-window-test --no-launch
```

In graphical window mode the debug kernel does not attach the debug-exit device.
After `OK_TEST_PASS`, the kernel draws a colored pixel marker in the framebuffer,
then enters an interactive debug shell and echoes keyboard input through the
kernel display path and serial console. Close the QEMU window when you want the
script to print its final result.

## Pass Signal

The debug kernel must print `OK_MODE debug`, `OK_DEBUG boot=complete`,
`OK_DISPLAY_TEXT`, and `OK_TEST_PASS`. The Python runner also verifies that
debug test points ran and that filesystem, EXT4, user-mode, and display checks
reported success. The current required coverage fields are `fs`, `simplefs`,
`ext4`, `user`, `display`, `gpu`, `input`, `posix`, `bus`, `usb`, `net`,
`shell`, and `modes`. Any non-zero exit code or missing marker is a failure.

## CI Coverage

GitHub Actions has one matrix job. For each architecture it:

1. Builds or restores the matching freestanding GCC/binutils toolchain.
2. Configures xmake with `-a <arch>`.
3. Builds `okernel`.
4. Runs `xmake test`; bootable system targets immediately boot the generated
   kernel in QEMU, while other targets run the freestanding compile profile.
