# SMP scaling WP0a: lock substrate

First package of the smp-scaling work (most kernel work off the big kernel
lock, plus a user-facing CPU hotplug interface). WP0a adds the substrate the
later packages lock with and changes no behaviour: every lock it adds is
unused by production code or nested inside the big kernel lock (BKL), and the
`OK_SMP_KERNEL` report keeps its format. Baseline: main 5815814 (module ABI
81); the ABI number is not bumped here.

## What it adds

- **Rank table** (`include/ok/smp/lock.hpp`). `LockRank` is the global order:
  class B serving ranks `big_kernel` 10, `vfs_namespace` 14, `ofd_position`
  16, `fs_provider` 18, `block_request` 20, `mm` 22, below every class A spin
  rank from `files` 30 to `driver_abi` 94. `lock_class()`,
  `top_half_floor` (`irq_router`) and `bottom_half_floor` (`device`) name the
  class and interrupt rules. The unused `scheduler`, `wait_queue`, `memory`
  and `leaf` enumerators are gone; `page_pool` replaces `memory`.
- **Locks.** `RankedSpinLock::lock_as<R>()` records one acquisition at a
  higher rank (the futex bucket pair). `UpLock<R>` never spins; it keeps the
  owner word and order checks in debug builds and one plain held byte in
  release builds, so `try_lock` fails and `owned_by_current_cpu` reads true
  while it is held in both modes, as on a `RankedSpinLock`. `FineLock<R>`
  picks the spin lock where `concurrent_locks` holds (`OK_ARCH_SMP_START` or
  `OK_HOSTED_LOCK_MODEL`) and the `UpLock` elsewhere; the `smp`
  `fine-lock-selection` test point checks the booted kernel's choice. `TryToken` is the
  unranked try-only token behind `hotplug_turn` (now used by the hotplug turn)
  and `housekeeping`; it stays out of the lock-debug record.
- **Gates** (`include/ok/smp/gate.hpp`). `ServingGate<R>` and
  `SharedGate<R>` follow the ext4 operation gate: same-CPU re-entry is
  `busy`, waiters spin SPINNING and serve their interrupts, the hold is order
  checked and recorded. The ext4 gate and `DescriptionPositionGuard` are not
  re-based: the ext4 gate checks mount and node identity inside its claim, and
  the position guard is an exported `RankedSpinLock` with section-specific
  waiting, so neither move is a pure refactor (left for the files package).
- **Checks.** `lock_debug_capacity` 12 and a per-CPU deepest nesting;
  `check_held`, `require_bkl` (counted as `exclusive_asserts`, reported as
  `bkl_asserts`), the I1/I2 class check `check_class_wait` (gate entry,
  `wait_for_interrupt`, `quiesce_root`, the hotplug waits) and the top-half
  rank check (`TopHalfScope` around `ok_interrupt_top_half`). Counters live in
  `check_cpus`; per-class acquisitions (debug) and contention (both modes) in
  `lock_stat_cpus`.
- **Mode bits.** `cpu_mode_kernel_free` (16), counted by `productive()`, and
  `cpu_mode_top_half` (32). The BKL frozen-check hook (`bkl_frozen_check`,
  `bkl_thread_frozen`) exists but nothing installs or asks it yet.
- **Runtime.** `started` is atomic; `runtime_failure` is stored under
  `runtime_status_lock` (rank `runtime_status`) and `fail_runtime` records a
  failure and stops the runtime in one hold, keeping today's overwrite
  semantics. The one exception is `RuntimeFailure::bkl_sink()`, a named
  `Status &` that the UDP and TCP operation bodies record release failures
  into; those writes skip the lock and are safe only while both families
  dispatch under the BKL (it replaces an implicit `Status &` conversion, so no
  other write can bypass the lock unnoticed). `runtime_idle` is an array of
  atomics; user-entry outcome counters are per CPU.
  `src/user/runtime/dispatch_policy.hpp` holds the `bkl_required` tables (one
  per Linux profile, one native), all `bkl` and consulted nowhere. Builds
  that define `OK_BKL_DISPATCH_FORCED` (`smp_sections=n`, and the debug
  private futex peer fixture, on any profile) force every row; every other
  build, UP profiles included, follows the tables.
- **Driver ABI.** `OkMutex` is an `atomic_flag` with the same try contract;
  `OkWaitQueue` and `OkSlabAllocator` run under one `driver_abi` leaf lock.
- **Evidence lines**, after the existing report lines once the runtime armed
  its checks, on every profile: `OK_SMP_KERNEL_CHECKS` (every check counter
  plus `max_lock_depth`), `OK_SMP_LOCKS` (`acquired:contended` per class since
  the checks were armed), `OK_SMP_WAKE`, `OK_SMP_MM`, `OK_SMP_AUDIT` (counters
  later packages feed; zero now) and `OK_SMP_CHECK_SITE` for a failed check.
  An SMP kernel built with `smp_sections=n` prints the check line as
  `OK_SMP_CHECKS`, because the OS stress gate rejects any `OK_SMP_KERNEL` text
  from such a kernel.

## Deviations from the specification

- `OK_SMP_CHECKS` in sections-off SMP builds (above).
- `fail_runtime` keeps the overwrite semantics of the old assignments instead
  of recording only the first failure; a first-failure recorder would change
  which status a failed runtime reports.
- `OK_SMP_HOTPLUG_USER` and `OK_SMP_AFFINITY_BROKEN` belong to the hotplug
  package. `window_lock_asserts`, `translation_writer_asserts`,
  `owner_asserts`, `queue_asserts`, `pin_asserts`, `admit_shadow_mismatch`,
  `quiesce_timeouts`, `audit_repairs` and `poll_audit_ready` are reported but
  nothing feeds them until their packages land.
- The UDP and TCP bodies' `bkl_sink()` writes bypass `runtime_status_lock`
  (above). Routing them through the lock needs a sink type threaded through
  the original-description guards and their hosted tests; the package that
  takes UDP or TCP off the BKL must do it.
- No `abi_dev = "smp-scaling.WP0a"` SDK manifest tag. Section 7 asks for one
  per package; the lead waived it for this series (one module ABI bump at
  the merge, and every gate relinks the OS against the fresh SDK). WP0a does
  change exported layouts at ABI 81: `LockRank` values, `LockDebugCpu`
  (capacity 12 plus `max_depth`), `OkMutex` (an `atomic_flag`, no longer
  copyable) and new inline globals in `ok/smp/lock.hpp`. An OS object built
  against the 5815814 SDK is therefore not compatible with this SDK and
  nothing rejects it; rebuild every OS object against this SDK.
- The gate and ext4 position-lock counts in `OK_SMP_LOCKS` record
  acquisitions (debug) only: the ext4 gate and the section path of the
  position guard keep their own waiting code, which does not count
  contention.

## Validation

All runs are QEMU TCG (`scope=hardware` means the emulated machine), on the
pinned SysToolchain QEMU 10.2; model tests print `scope=model`.

- Hosted: `scripts/test_smp_locks.py` (lock suite four ways: debug/release by
  `OK_HOSTED_LOCK_MODEL` on/off; gate suite debug/release with real host
  threads), `test_kernel_section_protocol.py`, `test_ext4_gate_concurrency.py`,
  `test_static_dispatch.py` (ten profiles: dispatch tables, flipped and
  forced rows, and `concurrent_locks` against a fixed list of SMP profiles
  with `OK_ARCH_SMP_START` passed as xmake passes it), the full
  `scripts/hosted_suite.py` including the ext4-fixture runners.
- Kernel: debug `okernel_image` and `xmake test` at `-smp 1` on all ten
  profiles; `-smp 2` and `-smp 4` on x86_64, aarch64 and rv64.
- OS (private copy of `wt/os-smp-scaling` 46b763d, unchanged): native distro
  boot to `OK_USERLAND scope=hardware acceptance=pass` on all ten profiles;
  `test_smp_stress.py` debug and release at 2 and 4 vCPUs on the three SMP
  profiles (release runs used a private release musl built from the pinned
  archive, because the shared release trees are empty); the futex probe, `--smp_hotplug_test=y
  --require-hotplug` and `smp_sections=n` at 2 vCPUs on each. Every run
  passed with every check counter and `lock_violations` at zero;
  `max_lock_depth` was 4 on the debug stress runs and 3 on the distro boots
  (release kernels keep no lock-debug record and report 0).
