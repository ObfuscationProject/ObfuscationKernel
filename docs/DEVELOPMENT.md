# Development Standard

The project language, comments, commit messages, and documentation are English.

## Code Style

- Use C++23.
- Prefer clear module boundaries over broad global state.
- Keep RTTI enabled.
- Use concepts/templates and static dispatch for architecture operations,
  scheduler policy selection and other high-frequency paths.
- Use virtual interfaces only at runtime device, filesystem and module-service
  boundaries where implementations can actually change at runtime.
- Preserve all ten architecture profiles; share mechanisms through typed policies
  instead of coupling generic code to x86_64.
- Return `ok::Status` or `ok::Result<T>` from kernel module operations.
- Keep `okernel` freestanding: no hosted STL containers, hosted heap allocation,
  exceptions, libc calls, or external C++ runtime dependencies. Kernel-owned
  physical/page/object allocation is permitted under the contract below.
- Keep comments short and reserved for non-obvious behavior.
- Do not introduce architecture-specific code into generic modules.

## Error Handling

Kernel APIs do not throw exceptions. Public module methods return `Status` or
`Result<T>`. A caller must check the result before dereferencing values.

## Kernel allocation contract

Kernel-owned allocation must be fallible and return `Status`/`Result`; do not
introduce implicit `new`, hosted allocation or exceptions. Each allocator must
define its owner, accounting limit, allocation context and reclamation condition.
Check sizes, alignment and capacity before publishing an allocation. Capacity
failure must leave existing owners and accounting intact; never silently truncate
a supplied physical memory map. Reservations override usable RAM declarations.

`FrameAllocator` metadata (range table, free bitmap, one `PageDescriptor` per
frame) is sized from the supplied map and lives in caller-owned storage; at boot
it is carved from discovered RAM. It has no blocking, reclaim or interrupt/SMP
synchronization; calls require a single serialized boot/model context.
`ChargeAccount` charges are per owner with an explicit limit; a charge failure
is distinct from exhaustion. Ownership handles carry the allocator identity and
a generation; callers must retire all mappings, references and I/O pins before
release. Bookkeeping does not prove those external lifetimes have ended. The
allocator must outlive its handles and cannot be copied or moved.
Reinitialization requires all frames to be released.

Production page/object allocation still requires boot RAM and reservation
discovery, runtime-sized metadata, explicit allocation contexts and charges,
reclaim/reserves and OOM policy, and synchronized reference/TLB/DMA retirement.
The default boot memory map and software VM/slab tests do not meet those gates.

## Architecture Work

Each architecture must provide:

- An `ArchTraits` specialization.
- A static `NativeInstructions<Architecture>` backend satisfying the architecture
  instruction concept; `ArchOperations` selects the configured target at compile time.
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
xmake f -c -y -m debug -a x86_64
xmake -y -b okernel
xmake qemu-test
```

## Repository boundaries

ObfuscationKernel exports an architecture/mode-specific SDK with native headers,
UAPI, core and boot libraries, linker script, image and manifest. ObfuscationOS
consumes these artifacts, including when linking its own early-boot extensions.
Do not introduce an embedded kernel checkout in the OS repository.

Core mechanisms, platform support, native extension interfaces, the basic
compositor and kernel diagnostics belong here. Distribution desktop/session
policy, external modules/drivers, Linux driver adapters and ordinary applications
belong in ObfuscationOS. Extension lifecycle callbacks are not a reason to make
the kernel depend on a distribution class or application name.

Model tests must identify their scope. Booting a test in QEMU does not turn
metadata/page-table/scheduler model checks into hardware execution tests. Keep
real trap delivery, address-space isolation, application execution and module
relocation as separate acceptance milestones.
