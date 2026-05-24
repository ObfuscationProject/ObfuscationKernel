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

Use the matrix tasks when validating architecture coverage from one checkout:

```sh
xmake profile-matrix
xmake qemu-matrix
xmake qemu-window-matrix
```

`profile-matrix` compiles the freestanding `okernel` static profile for every
supported architecture. `qemu-matrix` and `qemu-window-matrix` build and boot
the debug `okernel_image` target for every supported architecture.

The current bootable QEMU system test targets are `i386`, `x86_64`, `aarch64`,
`arm32`, `rv64`, `rv32`, `loongarch64`, `mips`, `mips64`, and `ppc`.

`xmake test` always has a freestanding profile test for `okernel` and runs the
`okernel_image` QEMU boot test for the configured architecture.

Every booted QEMU check also creates a temporary 16 MiB disk image and attaches
it as `virtio-blk-pci`. The kernel binds that PCI device as `virtio-blk0`, then
formats and exercises SimpleFS/EXT4 through the generic block-device interface.
The debug suite also exercises UDP and TCP loopback through the kernel network
stack, and the Python runner emits `OK_QEMU_NET_TEST` when that coverage passes.

## Visual Test

The windowed task defaults to the current architecture:

```sh
xmake qemu-window-test
```

Pass `-a` to run another architecture without permanently switching the
checkout:

```sh
xmake qemu-window-test -a rv32 --no-launch
xmake qemu-window-test -a ppc --no-launch
```

For every supported architecture, it builds and runs the same `kernel.bin` in
QEMU.

The visible pixel output comes from the kernel's own ramfb display path on
architectures whose QEMU machine exposes fw_cfg: x86/i386 use fw_cfg I/O ports,
while `aarch64`, `arm32`, `rv64`, `rv32`, and `loongarch64` use fw_cfg MMIO.
`mips`, `mips64`, and `ppc` use QEMU Malta/ppce500 machines without
fw_cfg/standalone `ramfb`, so their window path falls back to a serial VC while
the kernel still exercises its generic memory framebuffer and GUI compositor
state.
The script captures serial diagnostics and prints the test result only after
the QEMU window is closed. Use the headless validation form in environments
without a graphical display:

```sh
xmake qemu-window-test --no-launch
```

In graphical debug-test mode the kernel runs the same validation suite, prints
`OK_TEST_PASS` to the serial console, closes debug GUI surfaces, and then halts
or exits through the debug-exit path when one is attached. Non-test graphical
sessions route keyboard input through the focused GUI surface: `oksh` receives
text only while focused, and the file manager consumes simple navigation keys.
The ramfb backend scales the kernel's logical GUI framebuffer into the full
960x540 pixel surface.

For `aarch64`, `arm32`, `rv64`, and `rv32` window sessions, QEMU attaches
`virtio-keyboard-device` and `virtio-mouse-device`; the guest consumes their
legacy virtio-mmio event queues so keyboard input reaches the shell and mouse
motion updates the framebuffer pointer. x86/i386 use PS/2 keyboard and mouse
events from the PC platform while rendering through ramfb.

## Pass Signal

The debug kernel must print `OK_MODE debug`, `OK_DEBUG boot=complete`,
`OK_DISPLAY_TEXT`, and `OK_TEST_PASS`. The Python runner also verifies that
debug test points ran and that filesystem, EXT4, user-mode, and display checks
reported success. The current required coverage fields are `fs`, `simplefs`,
`ext4`, `user`, `display`, `gpu`, `input`, `posix`, `bus`, `usb`, `net`,
`shell`, `modes`, and `gui`. The bootable system targets advertise the same
QEMU capability set, so headless validation requires the same fields for all
supported architectures. Any non-zero exit code or missing marker is a failure.

## CI Coverage

GitHub Actions has one matrix job. For each architecture it:

1. Installs the QEMU packages needed by the matrix: `qemu-system-x86`,
   `qemu-system-arm`, `qemu-system-mips`, `qemu-system-ppc`, and
   `qemu-system-misc`.
2. Builds or restores the matching freestanding GCC/binutils toolchain.
3. Configures xmake with `-a <arch>`.
4. Builds `okernel`.
5. Runs `xmake test`, which immediately boots the generated kernel in QEMU for
   the configured architecture.
