# SMP scaling WP3: memory

Memory package of the smp-scaling work. Every memory path is still called
under the big kernel lock (BKL); WP3 changes no behaviour a user can see, it
makes the memory layer safe for the callers that later packages take off the
BKL. Baseline: WP0a (a10aeb9) on main 5815814 (module ABI 81); the ABI number
is not bumped here.

## What it adds

- **Allocator locks** at their global ranks (`ok/smp/lock.hpp`), each a
  `FineLock` (a spin lock on the SMP profiles and in the hosted race models,
  an `UpLock` elsewhere): `frame` (bitmap, counts, generations; `descriptor()`
  checks the exact handle under it), `slab`, `page_pool` (renames the former
  memory rank; quotas, counters and native descriptor fields), `object_pool`
  (one per pool), `shared_files` (memfd tables and ledger), `entry_stack`
  (`KernelEntryStackPool` records and per-CPU active slots) and `mm_retire`
  (the retirement quarantine's slot state). Reserve under the lock, work
  outside it: object runs are zeroed, entry-stack canaries written and backing
  cleared, and memfd frames staged outside their locks. Retirement pops a
  quarantine slot under `mm_retire` (reserved, or claimed busy) and destroys
  tables and releases frames after unlocking. `NativePagePool::check_caller`
  (the BKL check object pools made) is gone with the object pool lock.
- **Windows** (`ok/smp/bringup.hpp`, x86_64, aarch64 and rv64). Kernel windows
  (`KernelWindow`, odd `kernel_generation`, nesting per CPU, mode bit
  `cpu_mode_window`) around every backend user access; user windows gain the
  full fence. `quiesce_root` fences first, kicks user windows, waits for kernel
  windows without a kick, serves local interrupts while it spins, counts
  `quiesce_rounds` and asserts W2 (nothing above `mm`, as
  `wait_depth_asserts`). A lock taken inside a kernel window outside a top
  half counts `window_lock_asserts`.
- **Writer order** on the three backends: publish the entry (aarch64: break
  before make with a broadcast `TLBI VAAE1IS` when the output address
  changes), bump the TLB generation under `arch_spaces`, `quiesce_root`, and
  only then may the caller free what the old entry reached. `destroy_space`
  marks the slot retiring first. `SpaceTable` entries are atomic words
  (`std::atomic_ref`) with a `retiring` state; the generation doubles as the
  entry's sequence (changed first by release and acquire, reread last by
  `find`), so a lookup never mixes two lives of a slot. `TlbSpaceTag`'s
  generation is an atomic word. `enter_impl` validates its space lock-free
  inside the caller's user window (no `spaces_lock`). rv64 user copies walk
  the page tables instead of the runtime's mapping records. `Trap` gains
  `translation_changed`, set when the space's generation moved during the
  entry; the runtime resumes a fault on a page that is mapped for any data
  access again (the aarch64 break-before-make window).
- **No backend calls back into the runtime**: `WriteResolver` is gone from
  all ten backends; a withheld (copy-on-write) page fails a write like any
  unwritable one. `ok/user/uaccess.hpp` routes every runtime user access
  (`copy_from_user`, `copy_to_user`, `user_range`, `user_word`):
  a failure takes `mm` of the space's owner, resolves every withheld page of
  the range, retries up to four times and makes a final attempt holding `mm`
  (`uaccess_retries`, `uaccess_held`); a space no live process owns keeps its
  first result; inside a kernel section nothing is resolved (A6,
  `cow_refused`). `with_user_word` accesses one word inside its own window,
  never resolves and may run under a class A lock. User accesses count
  `class_asserts` under a class A lock. The runtime hook is a weak function
  (`native::resolve_user_access`) defined by vm.cpp under its qualified name.
- **`mm`**: `ProcessResources::mm` (`AddressSpaceGate`, a `ServingGate` at
  rank `mm` with `space_sequence` and `dead`). mmap, munmap, mprotect, brk,
  copy-on-write resolution (uaccess and page faults), fork's copy, the exec
  commit and process teardown run under it. `native_space` is published under
  the seqcount (`publish_native_space`, `read_native_space`). Translation
  writers on a published space without the owner's `mm` count
  `translation_writer_asserts`. `replace_native_pages` checks `mm` instead of
  the section drain.
- **Stage then build mmap**: a read-only plan under `mm`, the retirement slot,
  then the frames, shared-file aliases and file bytes staged without `mm`
  (`StagedRange`), then a second plan and the build from the current records
  under `mm`. brk holds `mm` across its call and stages under it.
- **Fork** withholds the parent's writes with one `hw::protect` (new on all
  ten backends; one quiesce on the SMP ones, `protect_pages`).
- **Per-load `ElfPrefix`**: each load reads its ELF prefix into a kernel-object
  run of its own (the static 128 KiB/512 KiB buffer is gone), the x86_64
  interpreter prefix too.
- Evidence (`OK_SMP_MM`): `kernel_windows`, `quiesce_rounds`, `cow_copied`,
  `cow_reused`, `retiring_rejects`, `protect_pages`, `uaccess_retries`,
  `uaccess_held`; `mm` in `OK_SMP_LOCKS`.

## Deviations from the specification

- `SpaceTable` and `TlbSpaceTag` keep plain fields accessed through
  `std::atomic_ref` instead of `std::atomic` members, so the growable tables
  of the UP profiles stay trivially copyable.
- The uaccess hook finds the space's owner by scanning the processes (the
  caller holds the BKL); the per-CPU caller binding comes with WP2. It checks
  `same_space` under the owner's `mm`.
- A genuine fault costs one more backend walk (the final attempt under `mm`).
- `Trap` grew by one byte (40 bytes on 64-bit profiles), an exported layout.
- With every quarantine slot taken, exec now refuses (ENOMEM) before it reads
  the ELF prefix, so before the ENOEXEC checks.
- Each exec takes up to 32 contiguous object frames (128 on PPC) for its
  prefix from the object reserve, where it used a static buffer.
- An object constructor that re-enters its own `ObjectPool` deadlocks on the
  pool lock in SMP release builds (debug builds report a self-deadlock and
  refuse); it returned busy before.
- futex still uses `uaccess::user_word` under the BKL (WP4c moves it to
  `with_user_word` under the buckets and `mm`).
- `copy_on_write_stats` stay plain counters (BKL-serialized); the per-CPU
  `OK_SMP_MM` counters are fed beside them.
- The hosted race targets of the xmake-based runners build without the
  sanitizer flags of `--sanitize`; the race models were also run once by hand
  under ThreadSanitizer (clean). The window model is not: ThreadSanitizer does
  not model the seq_cst fence pair and reports it.

## Validation

All runs are QEMU TCG on the pinned SysToolchain QEMU 10.2 (`scope=hardware`
means the emulated machine); model tests print `scope=model`.

- Hosted: the full `scripts/hosted_suite.py` (six shards, toolchain root and
  ext4 fixture) with the new runners `test_translation_windows.py` (poisoned
  frees, free-before-quiesce control) and `test_uaccess.py`; the frame, slab,
  page pool, object pool and entry stack runners with their race builds;
  `test_tlb_tags.py` (writer order); `test_native_vm.py` (with the
  mmap-versus-sibling staging model) and `test_native_exec_arguments.py` now
  on all ten profiles, `test_native_exec_eviction.py` on all ten.
- Kernel: debug `okernel_image` and `xmake test` at `-smp 1` on all ten
  profiles, `-smp 2` and `-smp 4` on x86_64, aarch64 and rv64; the native
  probes `test_native_{x86_64,aarch64,rv64}.py` at two CPUs
  (`remote_remap=pass`) and `test_native_{arm32,rv32,i386}.py`.
- OS (private copy of `wt/os-smp-scaling` 46b763d, unchanged): native distro
  boot to `OK_USERLAND scope=hardware acceptance=pass` on all ten profiles;
  `test_smp_stress.py` debug at 2 and 4 vCPUs and release at 2 on the three
  SMP profiles (release with private release musl sysroots built from the
  pinned archive: the shared release trees lack their sysroot); the futex
  probe, `--smp_hotplug_test=y --require-hotplug` and `smp_sections=n` at 2
  vCPUs on each. Every run passed with every check counter and
  `lock_violations` at zero; `max_lock_depth` was 5 in debug kernels.
- One x86_64 hotplug run recorded 2 failed cycles of about 530 (its log was
  overwritten before the reason was read); eleven further x86_64 hotplug runs
  of the WP3 kernels, seven of them instrumented to print the reason, and two
  of the a10aeb9 baseline had none.
