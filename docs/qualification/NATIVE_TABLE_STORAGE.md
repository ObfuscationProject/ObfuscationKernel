# Runtime-allocated address spaces, mapping records and shared files

Slice of `feat/mm-general` (WT1.1) that closes its second exit-gate item: no
fixed user memory pool remains in `include/ok/user` or `include/ok/memory`, and
the execution backends no longer keep 16 static address spaces of 1024 records.

## Contract

- Table storage: `execution::allocate_table_pages(pages, alignment)` and
  `release_table_pages` hand backends zero-filled runs from the native page
  pool's object reserve (`NativePagePool::allocate_object_run`). The address
  is both the supervisor pointer and the address the translation hardware
  walks. `FrameAllocator::allocate_contiguous` gained an alignment for ARM32's
  16 KiB first-level table.
- Backends: x86_64, i386, AArch64, ARM32, RV64 and RV32 allocate each space's
  fixed root tables and one leaf page per populated 2 MiB (4 MiB on RV32 and
  ARM32, where one page holds four coarse tables) when the space is created and
  release them when it is destroyed, leaves first. Table pages are checked
  against the window each kernel reaches untranslated. LoongArch, MIPS and PPC
  refill from the records and own no tables. Every backend keeps its slots in
  `SpaceTable` (`ok/arch/execution_spaces.hpp`): a slot table in table storage
  that doubles when full and is released with its last space. Handles keep
  root, slot and generation; generations come from one sequence that outlives
  the table, so a stale handle never names a later space.
  `OwnershipReport::table_pages` counts what a backend holds.
- Records: `MappingView` is one contiguous span in strictly ascending virtual
  address order (`valid()` checks the order, which also excludes duplicates;
  `find()` searches by page). `MappingPages` keeps the records and one
  retirement byte per record in one object run and exact frame tokens in a
  second. Both grow by doubling while unpublished; taking a view publishes the
  directory, after which insertion and growth are refused and only `replace`
  edits a record. The user window (`user_window_pages`) bounds the count. The
  runtime's retirement quarantine keeps its per-record ownership and pin state
  in the retirement bytes, and holds one entry per process plus one candidate.
- Reserve: the object reserve is the runtime objects' pages plus a sixteenth of
  the frames free at activation, at least 256. User pages never take reserved
  frames; objects never exceed the reserve, as `OK_NATIVE_FRAMES` reports.
- Shared memory files (SDK profile `bootcpu-volatile-sealed-size-write-eager-pages-v3`):
  the file table, each file's page index and the retirement ledger are object
  runs that grow on demand and are released once empty. The 16-bit handle slot
  bounds the file count; a file is refused only when its pages exceed the user
  quota or the pool runs out. The `capacity` and `max_pages` profile constants
  are gone.
- Software VM model: `LinearAddressSpace`, `PageTable`, `UserAddressSpace` and
  `VirtualMemoryManager` moved from `ok/memory/memory.hpp` to
  `tests/kernel/model/vm_model.hpp`; only the roadmap tests and test points use
  them, and `MemoryManager` no longer owns a model kernel space.

Fixed bounds that remain in those headers are not user memory pools:
`max_discovered_regions` bounds firmware input, and the object capacities in
`include/ok/user` (PTY pairs, futex requeue waiters and session processes tied
to `sched::max_processes`, `robust_list_limit`, user accounts) belong to their
subsystems. `sched::max_processes` (64) now bounds native processes, not the
backends.

## Verification

- Hosted: `test_native_page_pool.py` (object runs: alignment, zeroing, reserve,
  foreign and partial release), `test_physical_map.py` (aligned contiguous
  runs), `test_native_mapping_pages.py` (growth, publication pin, retirement
  bytes, failed-release quarantine), `test_mapping_view.py`, `test_native_vm.py`
  with the record quarantine and the object-reserve budget, and the shared-file
  owner tests in `tests/host/native_shared_files_tests.cpp` (more than sixteen
  files, an 80-page file, quota refusal, ledger growth under reserve pressure);
  all also under ASan/UBSan. The SDK contract tests admit profile v3 and refuse
  v2.
- QEMU, all ten profiles under the pinned QEMU 10.2.0: `xmake test`, and the
  paired ObfuscationOS distro booting natively with `qemu-distro
  --require-native` in the default copy-on-write and the eager fallback builds.
  `posix-check` now maps 1024 more pages where the former per-space record
  bound refused them, and still sees `ENOMEM` beyond the user window.

The slice is serialized boot-CPU code; table and slot growth takes no lock.
