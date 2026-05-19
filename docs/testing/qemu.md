# QEMU Testing

The current test model is bootless. `qemu_kernel` is a hosted debug entry that
calls the kernel's `ok_kernel_main` entry point. The debug kernel boots itself,
runs module-level checks, writes Linux-style boot lines through the framebuffer
driver, and emits `OK_*` diagnostics for the Python runner to validate.

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
xmake qemu-test -a aarch64
```

## Visual Test

The windowed task is also current-architecture only:

```sh
xmake qemu-window-test
```

It builds and runs `qemu_kernel`, generates a small bootable VGA text image from
the kernel's framebuffer text, and launches QEMU. The script prints the test
result only after the QEMU window is closed. In headless environments:

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
4. Runs `xmake qemu-test` for that same configured architecture.
