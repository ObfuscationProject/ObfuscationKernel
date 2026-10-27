# ext4 module layout (`refactor/ext4-layout`)

85 of the 87 ext4/JBD2 slice headers of `include/ok/fs/` are folded into eight
module headers under `include/ok/fs/ext4/`, and the 20 partial class bodies moved
to `include/ok/fs/ext4/detail/`. No declaration changed: each module is the
concatenation of its slices in include order with the slice-to-slice includes
removed. `include/ok/fs/ext4/README.md` maps every former header to its module.

## Layering

The name-based map in `docs/PLAN.md` Appendix A was cyclic between every pair
of modules (for example plans in `alloc` needed `txn`, and `txn` held the owned
transaction that needs everything). The modules are therefore cut by actual
include dependency into a strict stack:

`journal` → `layout` → `extent` → `alloc` → `dir` → `txn` → `recovery` → `mount`

Differences from Appendix A: JBD2 records, the replay executor and
`Ext4RecoveryBinding` form the bottom `journal` module, because the read-only
view depends on them; each mutation family's journal encoder stays with its plan
and witness (`extent`, `alloc`, `dir`); cold replay, recovery bindings, result
histories and orphan-chain capture form `recovery`; the owned transaction and
empty-orphan recovery join the mounted provider in `mount`. The include cycle
between `ext4_orphan_chain_capture.hpp` and `ext4_reclaim_composed_history.hpp`
and the include-order dependency of `ext4_regular_move_completion.hpp` disappear
inside their modules. `scripts/check_ext4_layout.py` now enforces the layering,
single ownership of every partial, the absence of old include paths and
standalone compilation of each module; the kernel CI runs it as `source-checks`.

The partials are template class bodies of SDK-exported classes
(`Ext4OwnedTransaction`, `Ext4MountedMetadata`, the zero-truncate ownership
validator). ObfuscationOS compiles them from the SDK, which exports `include/`
only, so they live in `include/ok/fs/ext4/detail/` rather than `src/fs/ext4/`.
`src/fs/ext4_rename_plan.hpp`, a forwarding header for an SDK43 path, is removed.

The two SDK contract marker headers, `ext4_directory_create_sdk_profile.hpp` and
`ext4_mounted_common_profile.hpp`, stay standalone. `tools/export_sdk.py` and the
OS consumer read them with a strict contract parser that refuses conditionals and
treats the digit separators used in the modules as malformed literals; keeping
markers small is the convention of every other `*_profile.hpp` header. The module
ABI stays at 74 on this branch; the bump is aligned with `feat/mm-general` at merge.

Six hosted models reached private members by wrapping one slice include in
`#define private public`. With modules the wrapped header is already included,
so these models (`ext4_empty_orphan_recovery`, `ext4_mounted_create`,
`ext4_reclaim_composed_history`, `ext4_reclaim_result_history`,
`ext4_zero_truncate_anchor_observation`, `ext4_zero_truncate_transaction`) are
built with `-fno-access-control`, as most hosted owner models already are.

## Equivalence evidence

Model-scope and QEMU-scope checks only; no physical hardware. Receipts are in
ObfuscationEvidence; each `result.json` names the tree it ran on.

| Commit | Receipts |
| --- | --- |
| `5372e50` (OS `084e2bf`) | `receipts/ObfuscationKernel@5372e50/refactor/ext4-layout/{kernel-ten-profile,object-identity,hosted-runners,os-consumer}` |

- Kernel: all objects of `okernel` and `okernel_image` for the ten profiles in
  debug and release were built from the folded tree and from the parent commit.
  After `--strip-debug` every object is byte-identical except the debug test
  units (`tests/kernel/debug/suite.cpp`, `points/test_point.cpp`,
  `roadmap/network_storage_tests.cpp`) and, on the rv32, rv64, loongarch64 and
  mips64 debug builds, `src/fs/ext4.cpp`, `src/fs/ext4_fs.cpp` and
  `src/rescue/shell/storage.cpp`. Recompiled to assembly with their exact xmake
  command lines, those are identical per symbol, except the test units on arm32,
  i386, ppc and one on rv64, whose `-O2` code differs and whose `-O0` code is
  identical. `kernel.elf` itself is not compared: xmake archives `libokernel.a`
  members in a nondeterministic order. `xmake test` and the `kernel-sdk` export
  pass on all ten profiles.
- Hosted units: 269 `tests/{host,native,fixtures,kernel}` translation units reach
  an ext4 header. Taken unchanged from the parent commit and compiled against the
  parent headers and against the modules (through old-path forwarding headers):
  84 objects are byte-identical, 135 are identical per symbol at `-O2`, 32 differ
  at `-O2` only by register allocation, speculative devirtualization or constant
  pool sharing and are identical at `-O0` (C1/C2 aliases merged; the extra symbols
  are unreferenced `std` concept-helper variables), 16 fail on the parent commit
  with the same diagnostics, and 2 that failed on the parent commit
  (`Ext4OwnedTransaction` instantiated before `Ext4MountedMetadata` was complete)
  now compile.
- Hosted runners: the 23 fixture-free ext4/JBD2 runners and the 8 clean-image
  runners (with a 32 MiB image made like the OS `create_linux_fixture.py`
  baseline) give the same status and result payloads on both trees; runners that
  already fail on the parent commit fail at the same step. The pinned runners'
  `HEADERS` tuples were already stale on the parent commit; they list the module
  closure now, and `test_ext4_inode_lifetime.py` passes with fresh pins.
- ObfuscationOS `refactor/ext4-layout` against the exported SDKs: `kernel-sdk-check`,
  `uapi-test`, `libc-test`, `distro_image`, `linux-driver-compat`, both rootfs
  images and `qemu-distro --require-native` pass on x86_64, arm32 and ppc;
  `compat-test` and `artifact-test` (1287 tests, run in two processes because the
  pure-Python `test_native_child_exit_lineage_guest` crashed the shared interpreter
  in some runs on both trees) pass on x86_64; the ext4 two-boot
  persistence test passes and its offline content is byte-identical to the
  parent SDK run. The ext4 `tests/native` units that compile standalone produce the
  same code against both SDKs.
