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
- `abi_version`, currently `kernel_module_abi_version == 1`;
- a capability mask declaring service export/use, kernel-process ownership,
  per-CPU workers, user ABI, driver ABI, or GUI behavior;
- `ModuleRestartPolicy`;
- `ModuleResourceBudget` for maximum threads, services, memory pages, and
  handles.

The manager rejects unsupported ABI versions, duplicate module names, dependency
cycles, missing required dependencies, missing required services, duplicate
service publication, capability mismatches, and resource budgets that cannot
cover the requested execution/threading model.

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
imports. `LinuxAbiSnapshot` records a mainline-tracking baseline, required
symbols, implemented symbols, and structure layouts so compatibility coverage
can be measured over time.

The current loader does not relocate or execute arbitrary external module text
yet. For the C++ OOP transition path, a post-boot loader can call
`OK_SYS_LOAD_MODULE`; the kernel reads an OKMOD package from the VFS, validates
its imports/exports plus `entry:oop`, binds it to a compatible C++ module ABI,
and starts that module through `ModuleManager::start_registered_module()`. This
is how ObfuscationOS loads its OS-side `system-gui` package from
`/boot/modules/system-gui.okmod` while the native relocating loader remains on
the roadmap.

## Test Coverage

Roadmap tests cover:

- single-module lifecycle and reverse stop order;
- missing dependencies, missing services, duplicate services, and dependency
  cycles;
- ABI-version and capability enforcement;
- restart-policy behavior and restart counters;
- OKMOD and Linux `.ko` metadata parsing;
- symbol import resolution and Linux ABI coverage snapshots;
- kernel-process module creation, per-CPU workers, and process reuse after
  restart;
- the built-in module graph used by the kernel debug suite.
