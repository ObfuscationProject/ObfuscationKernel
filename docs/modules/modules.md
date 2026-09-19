# Kernel Modules

`ok::ModuleManager` owns the kernel module graph. The current module layer is a
freestanding management and ABI-validation surface for built-in modules and
future loadable modules.

## Manifest Contract

Each module exposes a `ModuleManifest` with:

- `name`, `version`, and `module_class`;
- dependency, exported-service, and required-service lists;
- `ModuleExecution`, either `inline_core` or `kernel_process`;
- `ModuleThreading`, either `single_threaded` or `per_cpu`;
- `abi_version`, currently `kernel_module_abi_version == 2`;
- a capability mask declaring service export/use, kernel-process ownership,
  per-CPU workers, user ABI, driver ABI, or GUI behavior;
- `ModuleRestartPolicy`;
- `ModuleResourceBudget` for maximum threads, services, memory pages, and
  handles.

The manager rejects unsupported ABI versions, duplicate module names, dependency
cycles, missing required dependencies, missing required services, duplicate
service publication, capability mismatches, and resource budgets that cannot
cover the requested execution/threading model.

ABI 2 adds filesystem node lifetime, mutation and synchronization operations and
block-device flush support. Rebuild native extensions against the matching SDK;
ABI 1 objects cannot be reused. SDK manifests carry the same ABI generation and
the OS rejects a mismatched generation before linking.

## Lifecycle

`start_all()` sorts modules by dependency and then by init priority. A module
moves through `probe`, `init`, `start`, and service publication. Shutdown and
stop walk the started list in reverse order so dependents are stopped before
their providers.

Modules can publish services through `ServiceRegistry`. Callers query services
by stable string IDs such as `gui.compositor` and `gui.desktop`.

## Kernel-Process Modules

Modules with `ModuleExecution::kernel_process` are registered as
scheduler-visible background processes named `mod:<module-name>`. The GUI
module uses this path as `mod:kernel-gui`.

`ModuleManager::bind_kernel_process()` connects the manager to the scheduler and
architecture operations. Per-CPU modules request worker threads through their
manifest and must provide a resource budget large enough for the active CPU
count. Restarting a kernel-process module reuses its scheduler process record.

The debug shell can kill supervised `mod:*` records only from the `kernel`
debug user. The module supervisor recreates eligible records and logs the
restart to the console and `/tmp/kernel.log`.

## Image Metadata

`ModuleImageLoader` parses two metadata formats:

- `OKMOD`, a text metadata format used by tests for future native modules;
- Linux ELF `.ko` metadata, including `.modinfo`, exported/imported symbols,
  relocation counts, init/exit section presence, signatures, and architecture.

`ModuleSymbolRegistry` stores exported kernel symbols and resolves module
imports. Linux driver requirement baselines, symbol coverage tracking and structure
layout snapshots belong to the OS compatibility layer, not this kernel module
mechanism. ObfuscationOS provides them in `compat/linux/include/obos/linux/abi_snapshot.hpp`
and its own compatibility model tests.

The parser does not allocate, relocate, verify signatures or execute module text.
`Kernel::load_external_kernel_module()` rejects executable ELF images explicitly.
An optional distribution `KernelExtension` may accept an OKMOD configuration
package and activate a component already statically linked into the image. The
kernel has no desktop package names or concrete external module types. The OS
owns package lookup policy, implementations and integration tests, consuming only
exported SDK artifacts. The optional C factory `ok_boot_extension` selects that
in-process extension; the C++ callback interface requires the same SDK/toolchain.

`OK_MODULE_METADATA` reports metadata and lifecycle model coverage. In particular,
relocation records are not applied relocations, signature strings are not verified
signatures, and metadata parsing does not establish compatibility with a Linux
kernel release.

## Test Coverage

Roadmap tests cover:

- single-module lifecycle and reverse stop order;
- missing dependencies, missing services, duplicate services, and dependency
  cycles;
- ABI-version and capability enforcement;
- restart-policy behavior and restart counters;
- OKMOD and Linux `.ko` metadata parsing;
- native symbol import resolution;
- kernel-process module creation, per-CPU workers, and process reuse after
  restart;
- the built-in module graph used by the kernel debug suite.
