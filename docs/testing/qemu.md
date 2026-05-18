# QEMU Testing

The current test model is bootless. `qemu_smoke` is a normal binary that creates
an `ok::Kernel`, boots it with a synthetic memory map, and runs module-level
checks.

## Commands

```sh
xmake f -c --arch_target=host
xmake run qemu_smoke
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
xmake f -c --arch_target=aarch64 --toolchain=ok-aarch64-linux
xmake -y -b qemu_smoke
xmake run qemu_smoke
```

The runner maps architecture profiles to qemu-user binaries:

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

GitHub Actions has two separate cross-architecture jobs:

- `host-smoke` runs the native smoke binary and the full architecture profile
  sweep.
- `cross-toolchain-build` builds the freestanding GCC/binutils toolchains and
  compiles `okernel`.
- `qemu-user-smoke` installs distro Linux cross compilers and qemu-user, then
  runs `qemu_smoke` for direct binary execution.
