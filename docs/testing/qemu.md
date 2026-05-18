# QEMU Testing

The current test model is bootless. `qemu_smoke` is a normal binary that creates
an `ok::Kernel`, boots it with a synthetic memory map, and runs module-level
checks.

## Commands

```sh
xmake f -c --arch_target=host
xmake run qemu_smoke
```

The convenience task is safe to run even after configuring a freestanding
`ok-*-elf` toolchain. It temporarily builds the hosted smoke target with a
hosted/profile compiler and restores the previous xmake architecture/toolchain
selection afterward:

```sh
xmake qemu-test
xmake qemu-test -a x86_64
```

To compile and directly execute every architecture profile on the host compiler:

```sh
xmake arch-check -m debug
```

To build every installed freestanding architecture toolchain and check for
unresolved runtime symbols:

```sh
xmake freestanding-check --allow-missing
```

For an architecture user-mode test binary:

```sh
xmake qemu-test -a aarch64 --user
```

For a visible QEMU status window:

```sh
xmake qemu-window-test
```

`qemu-window-test` runs the architecture smoke matrix, generates a small
bootable VGA text image, and launches QEMU with a graphical display showing each
architecture result.

The qemu-user runner maps architecture profiles to qemu-user binaries:

| Profile | QEMU binary |
| --- | --- |
| `i386` | `qemu-i386` |
| `x86_64` | `qemu-x86_64` |
| `aarch64` | `qemu-aarch64` |
| `arm32` | `qemu-arm` |
| `rv64` | `qemu-riscv64` |
| `rv32` | `qemu-riscv32` |
| `loongarch64` | `qemu-loongarch64` |

## Pass Signal

The test must print `OK_TEST_PASS`. Any non-zero exit code or missing pass marker
is a failure.

## CI Coverage

GitHub Actions keeps smoke testing adjacent to each architecture build:

- `host-smoke` runs the native smoke binary and the full architecture profile
  sweep.
- `cross-toolchain-build` builds the freestanding GCC/binutils toolchains and
  compiles `okernel`, checks freestanding runtime symbol closure, and then runs
  the matching smoke test in the same matrix job.
