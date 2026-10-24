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
- `abi_version`, currently `kernel_module_abi_version == 69`;
- a capability mask declaring service export/use, kernel-process ownership,
  per-CPU workers, user ABI, driver ABI, or GUI behavior;
- `ModuleRestartPolicy`;
- `ModuleResourceBudget` for maximum threads, services, memory pages, and
  handles.

ABI69 is the development cohort for the shutdown lifecycle and exported object
layout changes. Rebuild native modules and distribution extensions with its exact
SDK. ABI68 binaries are incompatible. Native UAPI0.10.0 adds original-init scalar
power PREPARE/COMMIT; its queued acknowledgement does not certify clean shutdown.

The manager rejects unsupported ABI versions, duplicate module names, dependency
cycles, missing required dependencies, missing required services, duplicate
service publication, capability mismatches, and resource budgets that cannot
cover the requested execution/threading model.

ABI 2 added filesystem node lifetime, mutation and synchronization operations
and block-device flush support. ABI 3 extends the filesystem node virtual
interface for rename, hard links and directory path reconstruction, and adds
retained working-directory state to process resources. ABI 4 adds object-based
pathname resolution and changes physical-frame ownership/accounting layouts.
ABI 9 added fallible VFS metadata and child-lookup virtual slots; ABI 8 supplied
owned boot-register capture. ABI 10 added retained large-file descriptor state
and atomic CLOEXEC duplication. ABI 11 changed the exported Linux ProfileData
layout for pipe/seek; ABI 12 added sleep and ABI 13 added poll/open. Current
ABI 14 adds positional/select operations, select wait outputs and qualified
filesystem attribute records. The native UAPI remains0.4.0 through checked,
deterministic field projection. Rebuild native extensions against
the matching SDK; older generations cannot be reused. SDK manifests carry the
same ABI generation, and the OS rejects a mismatched generation before linking.
This native C++ ABI generation is separate from the public user-space UAPI
version and the SDK manifest schema.

The private ABI57 successor adds the AF_UNIX outbound buffer to the canonical
open-file description while preserving TCP ownership. Exported module, ext4,
UDP and TCP markers and the SDK manifest must all declare57; the exporter
and OS consumer reject stale56 and future generations. Its public UAPI is
0.7.0 and manifest schema1. Rebuild every native extension from the exact
architecture/mode/compiler SDK. Prior executed ABI generations remain
historical evidence, and metadata checks do not qualify runtime code loading.

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

## Canonical metadata ownership

The bounded [metadata-buffer owner](metadata-buffers.md) retains one exclusive
block session, immutable canonical blocks, read pins and discardable private
transaction versions. It does not publish journal commits or home writeback.

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

ABI58 adds bounded UNIX stream rights records, move-only prepared receive reservations and canonical queue-reference accounting. It retains the earlier TCP, UDP and mounted ext4 mechanisms. Immediate native/Linux message adapters and hosted models do not establish blocking/general SCM_RIGHTS or an isolated desktop. SDK export and the OS consumer require matching ABI58 markers.

The private ABI61 successor reserves matching module and SDK markers for bounded
legacy ext4 writes on checksum-v3 media with an optional revoke bit. Actual
active revoke records remain refused. Linux UAPI remains 0.7.0; this source
change does not qualify target execution or persistent Notes durability.
