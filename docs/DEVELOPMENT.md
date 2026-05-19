# Development Standard

The project language, comments, commit messages, and documentation are English.

## Code Style

- Use C++23.
- Prefer clear module boundaries over broad global state.
- Keep RTTI enabled.
- Use virtual interfaces for runtime hardware, driver, filesystem, and policy
  variation.
- Use concepts for compile-time extension contracts.
- Return `ok::Status` or `ok::Result<T>` from kernel module operations.
- Keep `okernel` freestanding: no hosted STL containers, heap allocation,
  exceptions, libc calls, or external C++ runtime dependencies.
- Keep comments short and reserved for non-obvious behavior.
- Do not introduce architecture-specific code into generic modules.

## Error Handling

Kernel APIs do not throw exceptions. Public module methods return `Status` or
`Result<T>`. A caller must check the result before dereferencing values.

## Architecture Work

Each architecture must provide:

- An `ArchTraits` specialization.
- An `ArchOperations` implementation or adapter.
- Interrupt entry and return strategy.
- User-mode entry strategy.
- Memory-management constants and page-table model.
- A QEMU debug test path.
- Documentation under `docs/arch/`.

## Tests

Every new module feature needs debug-kernel coverage that is reached through
`kernel_main` and validated by `xmake qemu-test` for bootable system targets.
Architecture features need either direct debug test coverage or a documented
qemu-system test until that architecture has a bootable `kernel.bin`.

Debug-only test points live behind `OK_ENABLE_TEST_POINTS`, which xmake defines
only in debug mode. QEMU tests require debug mode and fail if the kernel does
not emit `OK_MODE debug` and a non-zero `debug_test_points` count.

Before submitting architecture-sensitive changes, run:

```sh
xmake f -c -m debug -a x86_64
xmake toolchain-check
xmake -y -b okernel
xmake qemu-test
```
