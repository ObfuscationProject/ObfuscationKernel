# Hosted runner suite

Slice of `chore/hosted-runners` (Kernel issue #5). Until the P1 review only the
freestanding profile compile and the QEMU boot ran in CI; none of the 198
runners under `scripts/test_*.py` did, and 63 of them no longer built.

## Repairs

Shared causes, fixed in the runner scripts and test fixtures:

- 80 runner source lists gained `src/arch/terminal_power.cpp`
  (`arch::preflight_terminal_power_action` for `src/core/native_shutdown.cpp`);
  lists that reach the page pool, VFS sockets or the UDP service gained
  `src/memory/memory.cpp`, `src/fs/vfs.cpp`, `src/net/native_udp.cpp` or
  `src/syscall/syscall.cpp`.
- 20 test-local execution mocks gained `arch::execution::ownership_report()`; the
  shutdown model's report uses designated fields (`table_pages` had been
  receiving its `executing` flag).
- `src/user/runtime/entry.cpp` asserts the `ok_msghdr`/`ok_cmsghdr` layout only
  when the compiler word matches the profile word. A hosted model of a 32-bit
  profile on a 64-bit host decodes target bytes through `UnixMessageCodec`;
  every real target keeps the assertion.
- Snapshot runners: five runners copied the whole checkout plus the sibling
  ObfuscationOS tree and refused any untracked link; they now copy `include/`,
  `src/` and `tests/`. Runners with hand-picked header lists copy the quoted
  include closure of their inputs (`scripts/hosted_closure.py`), which brings in
  `ok/user/native_signal_config.hpp` (committed since P0; the lists predated its
  includes) and `src/sched/*.inc`.
- Stale expectations behind the link failures: futex and robust-list fixtures
  now create real mapping records (shared-domain keys resolve through them),
  sleep interruption maps its page through the pool, exec fixtures provide a
  hosted entropy source for `AT_RANDOM`, thread-I/O consumes the cancelled
  continuation before its next call, the poll-object reserve uses the
  RAM-scaled `kernel_reserve_pages`, two tests construct `Image` from a
  retirement reservation, the o32 delivery check uses an unassigned flag bit
  (`SA_NOCLDSTOP` is accepted), compile probes pin module ABI 76 and the power
  and mount-info wire checks expect UAPI 0.16.
- `test_linux_bootstrap.py` takes `--toolchain-root`.

## Suite

`scripts/hosted_suite.py` (`xmake hosted-test`) classifies every runner:

| Class | Count | Runs |
| --- | --- | --- |
| self | 124 | always |
| toolchain | 16 | with `--toolchain-root` (ten SysToolchain `<triple>/` trees) |
| fixture | 5 | with `--ext4-fixture`: `scripts/make_clean_ext4_fixture.py` builds the clean journalled image with e2fsprogs (debugfs `jo -c -v 3` gives the CRC32C v3 journal a first Linux mount used to) |
| inputs | 42 | never; `--list` names the images, oracles, OS-built ELFs or SDKs each needs |
| quarantine | 11 | with `--quarantined`; each has a recorded reason |

Quarantined on this branch:

| Runner | Reason |
| --- | --- |
| `native_signal_exec` | fatal modes: exec retires robust lists and clear-TID only after the close-on-exec descriptors retire; Linux runs `mm_release` before `do_close_on_exec` |
| `native_poll_objects` | mapping records require their exact pool frame; the kernel-page admission case must be rebuilt on frame tokens |
| `native_shutdown_runtime` | shared mode: hosted memfd `ftruncate` fails since shared memory files use the page pool object reserve |
| `native_child_wait_signal`, `native_process_signal_waits` | the syscall-origin capture of the hosted child-wait trap is refused |
| `linux_select` | assumes console descriptors 0–2; the hosted process opens its model file as fd 0 |
| `linux_udp_sockname`, `linux_udp_signal` | capture/decode model checks fail |
| `ext4_directory_mode`, `ext4_journalled_mode`, `ext4_transaction` | fail against the generated clean image; not yet compared with the Linux-mounted `baseline-clean.img` |

CI (`.github/workflows/kernel.yml`): job `hosted-suite` runs the self and
fixture classes in eight shards; every `build-and-test` job runs
`xmake native-probe`, the configured architecture's QEMU execution probe
(`scope=hardware`; mips64 has none) with the SysToolchain package's compiler and
QEMU. Toolchain runners that need all ten cross toolchains at once run in the
workspace lane.

## Results

Host g++ 13.3, SysToolchain 0.1.0 (GCC 14.2.0, QEMU 10.2.0), this branch:
self 124/124 and toolchain 16/16 pass (`--jobs 16`, 25 minutes), fixture 5/5
pass. Baseline at `6b5c37f` in a fresh worktree: 73 pass, 73 fail, 52 need
inputs (a fresh worktree also lacks the ignored `toolchains/` directory and
generated `tests/fixtures/jbd2_v3.bin` the review run had).

Not covered yet: the 15 `tests/host/run_*.py` drivers (they take slice images
from receipts, like the `inputs` class) and the 42 `inputs` runners, which need
artifacts this repository does not build.

Passing this suite is hosted model and QEMU probe evidence only: no guest
userland, SMP or physical hardware claim.
