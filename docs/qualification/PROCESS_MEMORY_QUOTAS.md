# Per-process memory quotas

Slice of `feat/mm-general` (WT1.1). Native processes are charged for the user
pages they map and bounded by their own `RLIMIT_AS`; `RLIMIT_DATA` bounds the
brk heap. Before this slice the only bounds were the global 1024-frame pool and
the backend's 1024-record per-space mapping capacity.

## Contract

- Charge: the process's mapped user pages. Native VM is eager, so every mapped
  page is backed; a shared anonymous page counts for each process that maps it.
  Mapping metadata blocks are kernel objects charged to the page pool's object
  reserve, not to the process.
- Quota: `memory_limits.address_space.current / page_size` pages, admitted in
  `native_mmap` (anonymous, file, fixed and brk growth) and for the replacement
  image at exec, before any existing mapping changes. brk additionally checks
  `memory_limits.data.current` against the heap size.
- Inheritance: fork copies the limits; exec keeps them.
- Interface: native `getrlimit(resource, old)` and
  `prlimit64(pid, resource, new, old)` for resources 2 and 9, pid 0 or the
  caller (otherwise `ESRCH`). `old` is written before `new` is applied. A soft
  value above the maximum is `EINVAL`; raising the maximum requires effective
  UID 0. Lowering below current usage is allowed.

The global page pool quota and exhaustion still apply independently: a process
inside its quota can still receive `ENOMEM` when discovered RAM runs out.

## Verification

- Hosted: `scripts/test_process_resources.py` (resource lookup, page admission,
  update rules, fork inheritance).
- Native QEMU guests through the paired ObfuscationOS worktree
  (`chore/sdk75-consumer`): `posix-check` lowers its own `RLIMIT_AS` soft limit
  to 3 MiB, below both the mapping capacity and the page pool, runs its shared
  anonymous pressure loop until `ENOMEM`, checks the readback, restores the
  limit and maps the refused size again, then prints
  `OK_VM_QUOTA scope=hardware resource=address-space refused=enomem restored=pass`.
  `qemu_distro.py --require-native` requires that marker.

This is QEMU execution on the boot CPU. It does not cover RSS accounting,
overcommit, memory cgroups or limits on Linux-personality processes.
