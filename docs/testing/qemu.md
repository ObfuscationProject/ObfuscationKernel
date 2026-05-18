# QEMU Testing

The current test model is bootless. `qemu_smoke` is an xmake test binary that
creates an `ok::Kernel`, boots it with a synthetic memory map, and runs
module-level checks.

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

It builds and runs `qemu_smoke`, generates a small bootable VGA text image, and
launches QEMU with the current architecture test result. In headless
environments:

```sh
xmake qemu-window-test --no-launch
```

## Pass Signal

The smoke binary must print `OK_TEST_PASS`. Any non-zero exit code or missing
pass marker is a failure.

## CI Coverage

GitHub Actions has one matrix job. For each architecture it:

1. Builds or restores the matching freestanding GCC/binutils toolchain.
2. Configures xmake with `-a <arch>`.
3. Builds `okernel`.
4. Checks freestanding link closure with the architecture linker and `nm`.
5. Runs `xmake test` for that same configured architecture.
