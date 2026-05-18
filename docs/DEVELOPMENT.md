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
- A QEMU smoke path.
- Documentation under `docs/arch/`.

## Tests

Every new module feature needs a host smoke assertion first. Architecture
features need either qemu-user coverage or a documented qemu-system test until
they can be run without a bootloader.

Debug-only test points live behind `OK_ENABLE_TEST_POINTS`, which xmake defines
only in debug mode. Release builds must keep `debug_test_points=0` in the smoke
output.

Before submitting architecture-sensitive changes, run:

```sh
xmake arch-check -m debug
xmake f -c -m release --arch_target=host
xmake run qemu_smoke
```
