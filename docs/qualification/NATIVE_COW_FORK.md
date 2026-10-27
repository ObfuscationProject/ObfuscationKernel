# Copy-on-write native fork

Slice of `feat/mm-general` (WT1.1). Native `fork` shares private pages
copy-on-write. The xmake option `native_cow_fork` (default on, defines
`OK_NATIVE_COW_FORK`) selects this; `--native_cow_fork=n` keeps the eager
fallback, where fork copies every private page as before.

## Contract

- Records: `PageMapping::copy_on_write` keeps write access a record is entitled
  to while its `flags` withhold `page_write` from the translation. Backends
  ignore the bit. `intended_access()` is the access reported to `MemoryMap`
  (and so to quota and access checks); `settle_access()` withholds write only
  while a private frame has more than one reference.
- Frames: fork gives the child one more reference on each private frame
  (`NativePagePool::retain_copy`). Such a frame is never classified shared, so
  `retain_shared` refuses it and shared anonymous or shared-file frames keep
  their existing alias path. After the child's address space exists, the parent
  withholds write from every private writable record whose frame is now shared
  and re-synchronizes each translation with `execution::remap`.
- Resolution: a write to a withheld page either regains write access in place,
  when the faulting space holds the last reference, or moves the record to a
  private copy of the frame (`MappingPages::replace`) and drops one reference
  on the original. Both cases call `remap` for that one record. Resolution runs
  for
  - user write faults: the native loop resolves a `page_fault` trap at the
    fault address and re-enters the thread at the faulting instruction;
  - kernel writes: every backend's `user_range(..., write)` consults the write
    resolver for a readable page that fails the write check, so
    `copy_to_user`, signal frames, robust lists, futex keys and entry stack
    checks see a writable page after resolution.
- `mprotect` settles each changed record against its frame's reference count,
  so adding write to a shared private frame withholds it, and removing write
  clears the withheld state.
- Charges are unchanged: a process is charged for every page it maps, shared
  or not. A frame is only allocated when a write resolves, so frame exhaustion
  appears there: a user fault is then contained as `SIGSEGV`, a kernel write as
  `EFAULT`. There is no reclaim or OOM policy.
- A failed `remap` leaves a translation that disagrees with its record. Both
  fork and resolution treat it as a runtime failure and stop native execution.

`remap` per backend: x86_64/i386 rewrite the PTE (native entry reloads CR3);
AArch64 performs break-before-make (entry invalidates the TLB); ARM32 and
RISC-V rewrite the leaf entry (entry runs `TLBIALL`/`sfence.vma`); LoongArch,
MIPS and PPC refill from the records, which native entry invalidates, and PPC
publishes a new executable frame to the instruction cache.

`OK_NATIVE_COW version=1 shared=<n> copied=<n> reused=<n>` follows the
`OK_NATIVE_FRAMES`/`OK_NATIVE_SHARED_FILES_LEDGER` pair in copy-on-write builds
only: records shared at fork, writes resolved by copying, and writes resolved
in place by the last owner. The ObfuscationOS consumers of SDK 75 serial output
(`qemu_distro.py`, `test_musl_userland.py`, `test_shared_files.py`,
`observe_shared_write_seal_qemu.py`, `object_pressure_evidence.py`) select
their records by prefix or exact adjacency and admit the line. The validators
that admit only an exact set of kernel lines inside a command span belong to
SDK 31 receipts: they require an ABI 31 SDK and already reject the
`OK_NATIVE_SHARED_FILES_LEDGER` line that every current build emits.

## Verification

- Hosted model: `scripts/test_native_vm.py [--sanitize]` builds the actual VM
  code twice, with `OK_NATIVE_COW_FORK` and eager; the COW build runs fork
  sharing, parent withholding, copy and in-place resolution, shared and
  read-only pages that do not resolve, `mprotect` settling, a failed `remap`
  and teardown reference counts against a record-reading mock backend.
  `test_native_page_pool.py` covers `retain_copy`; `test_native_mapping_pages.py`
  covers `replace`. Other hosted runners that compile `runtime.cpp` define
  their own flags and so model the eager fallback.
- Native QEMU guests through the paired ObfuscationOS worktree
  (`chore/sdk75-consumer`) with kernel SDKs exported from the default and the
  `--native_cow_fork=n` configurations: `posix-check` runs its existing fork tests and
  a bidirectional copy probe, where parent and child each write a still-shared
  private page by CPU store and by a kernel copy (`read` from a pipe) and
  observe only their own values; it prints
  `OK_VM_FORK_COPY scope=hardware parent_store=pass parent_kernel_copy=pass child_kernel_copy=pass child_store=pass`.
  `qemu_distro.py --require-native` requires that marker in both builds.
- Kernel CI builds the eager fallback image on every profile after exporting
  the default SDK.

This is QEMU execution on the boot CPU with one executing thread at a time. It
does not cover TLB shootdown, concurrent faults, page reclaim or swap.
