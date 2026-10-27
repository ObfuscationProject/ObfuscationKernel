# Native runtime split, signal headers and rescue gate

WT1.4 `refactor/user-runtime-split`, qualified on branch
`claude/p1-user-runtime-split-857ba0` against its base `main@7ddd8bb`:

| Commit | Change |
| --- | --- |
| `1dab3c3` | Hosted runners compile `src/rescue` instead of the removed `src/apps` |
| `2ac51d4` | Runtime private header, inline namespace, handlers extracted from `dispatch()`/`start()` |
| `0d62887` | `src/user/runtime.cpp` split into the units mapped in `src/user/README.md` |
| `69b73c2` | Per-architecture signal headers collapsed into `include/ok/arch/<arch>/signal.hpp` |
| `f5fb15f` | Rescue applications built only with `kernel_gui` or in debug builds |

The split and the header collapse move code without changing behavior. The
rescue gate changes release builds without `kernel_gui`: they link
`src/rescue/disabled.cpp`, whose debug shell, file manager and task manager
behave as never opened, so their diagnostics are unavailable there. The
`Kernel` layout and the SDK symbols do not change.

## Results

- Every runtime unit compiles for all ten profiles in debug, release and both
  private-fixture configurations (`0d62887`, `f5fb15f`; `runtime.cpp` alone at
  `2ac51d4`).
- All ten debug kernels pass `xmake test` at `0d62887`, `69b73c2` and `f5fb15f`
  (x86_64, arm32 and ppc at `2ac51d4`). All ten release kernels boot to
  `OK_KERNEL_BOOT ... status=ready` at `7ddd8bb`, `0d62887`, `69b73c2` and
  `f5fb15f`. No debug or release kernel object at `0d62887` or `f5fb15f` has an
  `.init_array` section.
- ObfuscationOS `658b18b`, unmodified, accepts the exported debug SDK of
  `7ddd8bb`, `0d62887`, `69b73c2` and `f5fb15f` and boots native userland with
  `acceptance=pass shell_pipeline=pass` on x86_64, arm32 and ppc. At `f5fb15f`
  its release `kernel-sdk-check` and distro image, including the system-gui
  module, also link against the release SDK.
- Hosted runners: 58 pass at `7ddd8bb` and 69 at `1dab3c3`, `0d62887`,
  `69b73c2` and `f5fb15f`, with no status change after `1dab3c3`. Compared with
  `1dab3c3`, failing runners report the same compiler errors and unresolved
  symbols, with one exception: at `0d62887` and later, `native_poll` and
  `linux_rename` also report an unresolved `arch::execution::ownership_report`.
  Both already failed to link at `1dab3c3`. In a single translation unit GCC
  dropped `shutdown_report` because hosted builds never record a shutdown
  owner; with the owner shared between units the function is kept. Kernel
  images always link an execution backend. The `f5fb15f` receipt's
  `failures_differing_from_baseline` lists twelve runners because its
  signatures keep xmake's failed link command line, whose object list now
  names `src/rescue/disabled.cpp.o`. Without that line only these two differ.
- Release objects of `69b73c2` match `0d62887` after stripping debug information
  and `.comment`. Only the mips64 `signals.cpp.o` is not byte-identical: it
  differs in section emission order and local label numbering, and all 134 of
  its functions disassemble identically once addresses and labels are
  normalized.
- Release `.text` without `kernel_gui` at `f5fb15f` against `7ddd8bb`: aarch64
  -91K, arm32 -92K, i386 -95K, loongarch64 -103K, mips -113K, mips64 -122K, ppc
  -137K, rv32 -86K, rv64 -76K, x86_64 -104K. The split alone (`0d62887`) changes
  it by -6.4K (rv32) to +44.3K (ppc). About 26K of the ppc figure is fill before
  the 64 KiB-aligned exception vectors; the rest is code no longer inlined
  across units and unwind tables on x86_64, loongarch64 and ppc.

## Receipts

Paths are relative to the ObfuscationEvidence repository (commit `25a2088`).
Images and exported SDKs are under the unversioned `archive/receipts/` path
named in each receipt's `ARCHIVE` file.

| Commit | Receipt |
| --- | --- |
| `7ddd8bb` | `receipts/ObfuscationKernel@7ddd8bb/main/{kernel-ten-profile,os-consumer,hosted-runners}` |
| `1dab3c3` | `receipts/ObfuscationKernel@1dab3c3/claude/p1-user-runtime-split-857ba0/hosted-runners` |
| `2ac51d4` | `receipts/ObfuscationKernel@2ac51d4/claude/p1-user-runtime-split-857ba0/kernel-fast-lane` |
| `0d62887` | `receipts/ObfuscationKernel@0d62887/claude/p1-user-runtime-split-857ba0/{kernel-ten-profile,os-consumer,hosted-runners}` |
| `69b73c2` | `receipts/ObfuscationKernel@69b73c2/claude/p1-user-runtime-split-857ba0/{kernel-ten-profile,os-consumer,hosted-runners}` |
| `f5fb15f` | `receipts/ObfuscationKernel@f5fb15f/claude/p1-user-runtime-split-857ba0/{kernel-ten-profile,os-consumer,hosted-runners}` |

Each `result.json` names the tree a command ran on and its relation to the
commit: an exact `git archive`, or blob-identical except listed documentation
or hosted-runner scripts the campaign does not read. `commands.txt`,
`logs/compile-flags.json` and the recorded QEMU argv reproduce the commands.

## Reproduce

Per profile, with SysToolchain 0.1.0 and its QEMU 10.2.0:

```sh
xmake f -c -y -m debug -a <arch> --systoolchain_dir=<systoolchain>/<triple> --qemu_binary=<qemu>
xmake -y -b okernel_image
xmake test
```

Release images use `-m release` (add `--kernel_gui=y` for the gated
applications). The OS lane exports `kernel-sdk`, configures ObfuscationOS with
`--kernel_sdk=<kernel>/build/sdk/<arch>/<mode>`, builds `kernel-sdk-check`,
`distro_image` and `rootfs-simplefs`, and runs
`xmake qemu-distro --fs=simplefs --require-native --timeout=120`. Hosted
runners are `scripts/test_<name>.py [--output <fresh-path>]`.

## Limits

QEMU runs execute on the boot CPU; none is physical hardware. Native userland
was booted on three profiles only; the other seven run the kernel debug test
points, which never start native userland. Release boots check readiness
without an OS extension. Hosted runners are model tests, and most of the 74
that fail do so for reasons unrelated to WT1.4, such as missing architecture
backends or fixtures. CI builds debug only, so `src/rescue/disabled.cpp`
compiles only in local release builds such as these receipts. The exported
header set changed at `69b73c2`; the module ABI bump is due at merge.
