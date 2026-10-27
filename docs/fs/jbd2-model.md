# JBD2 read-only codec and replay-plan foundation

The `ok::fs::jbd2` codec in `include/ok/fs/ext4/journal.hpp` implements a
bounded, freestanding C++23 **host-model foundation** for the storage plan. It
parses an immutable journal snapshot and returns an ordered list of candidate
replay actions after commit and revoke validation. It does not issue disk
writes, execute recovery, checkpoint a transaction, expose a filesystem root, or
admit a journaled ext4 mount. Existing ext4 admission remains unchanged. This is
not native recovery acceptance.

## Admitted format

| Component | Supported subset |
| --- | --- |
| Superblock | Version 2, at least 1024 supplied bytes; checksum covers exactly the first 1024 bytes with `s_checksum` zeroed |
| Features | Exactly checksum v3, optionally revoke and/or 64bit; compatible and read-only-compatible masks must be zero |
| Block sizes | 1024, 2048 and 4096 bytes; every record/payload read is exactly one configured block |
| Journal topology | One user, no dynamic superblock, no fast-commit area; caller supplies a validated journal mapping and expected UUID |
| Tags | 16-byte v3 tags; 32-bit targets with zero high field unless 64bit is enabled; full CRC32C payload checksums |
| Descriptor UUIDs | First tag must carry the matching UUID; later tags may repeat it or use SAME_UUID |
| Flags | ESCAPE, SAME_UUID, LAST_TAG; deleted tags and unknown flags are rejected |
| Revoke | Checked used-byte count, 32-bit or 64-bit big-endian entries, whole-block CRC32C |
| Commit | Whole-block CRC32C with the first checksum word zeroed; v3 checksum type/size, padding and unused checksum words must be zero; valid nanoseconds |
| Ordering | Multiple descriptors, repeated target blocks, optional revoke-only/empty transactions, ring wrap and transaction-ID wrap |

CRC32C uses reflected polynomial `0x82f63b78` and the raw CRC state without a
final complement. The superblock starts with seed `0xffffffff`; record checksums
use `crc32c(0xffffffff, journal_uuid)`. Payload checksums additionally include the
big-endian four-byte transaction ID before the payload. Descriptor/revoke tails
are zeroed logically while hashing their entire block. Commit checksum offset
16 is zeroed logically; checksum computation never modifies input bytes.

An escaped payload must begin with four zero bytes and carries an action flag
to restore `0xc03b3998` during a future executor. An unescaped payload cannot
start with that magic. This implementation only records the restoration intent.

Checksum v1/v2, no-checksum journals, async commit, fast commit, shared journals,
dynamic superblocks, external-journal binding, journal creation/mutation, and
larger block sizes remain unimplemented. Nonzero journal error state is rejected.
Unknown features are not ignored. The codec does not derive journal identity
from ext4 or prove that the caller's mapping is an internal journal; that
filesystem integration remains a separate gate.

## Plan API and ownership

`Codec::open` validates the superblock against the supplied available journal
block count and expected UUID. It checks Linux's minimum total journal length
of 1024 blocks, nonzero first log block, start bounds, and a ring shorter than
`2^31` blocks. A nonzero `s_start` selects the first scan block. A zero `s_start`
returns an explicitly empty-log plan, **not a clean-filesystem claim**.

`build_replay_plan` takes the validated codec, a statically dispatched
`read_block(u32, span<u8>) -> Status` provider, a `TargetDomain`, and caller-owned
`PlanStorage`. The reader must fill every successful read and expose a stable,
immutable snapshot for the entire plan lifetime. Read failures propagate.

The two block buffers, action array and revoke array must be disjoint. They are
fallible caller-provisioned scratch, not globally fixed pools. The result refers
to the action array and remains valid only until it is reused. Scratch is
unpublished and unspecified after any error; callers must check the returned
`Result` and must not consume partially filled arrays. The header allocates no
heap storage and invokes no hosted runtime.

`TargetDomain` contains the filesystem block count and an explicit span of
permitted metadata block ranges. Every range is checked without overflowing;
every replay and revoke target must fall within the domain. Callers must derive
these ranges from a validated filesystem, enforce ext4 address-width limits,
and exclude physical journal storage. The codec cannot manufacture those
filesystem facts. Consequently an arbitrary caller-supplied range is not proof
that a real device is safe to recover.

The scan validates record framing, all descriptors and payloads, revokes and
commits. It records commit boundaries before returning any usable plan. A final
uncommitted transaction's data **and revoke effects** are discarded. The plan
retains `revoke_records`, the number of validated active revoke blocks, including
empty blocks and uncommitted records. This count is distinct from
`revoked_actions`, which counts only suppressed committed actions. Validated stale
boundary records are not active. Bounded ext4 canonical writers reject every
active revoke record, while this general model still applies revoke semantics.
Among the remaining
committed records, a revoke suppresses the same target in its own or an earlier
transaction; it does not suppress later reuse. Monotonic traversal ordinals
implement this rule across `0xffffffff -> 0` transaction-ID wrap. Surviving
actions retain descriptor/tag order, including multiple writes to the same
block. Even payloads later suppressed by a revoke are checksummed; this is
stricter than Linux's replay loop.

Scanning terminates only on an all-zero block or a structurally/checksum-valid
older record. A future or half-range-ambiguous transaction ID is an error.
Nonzero unrecognized framing, malformed UUID/tag tails, a bad commit, and any
checksum error fail the entire plan. This deliberately does not implement
Linux's commit-time heuristic for accepting some stale or torn tail records.
It may therefore reject recoverable Linux journals; it never advertises the
full Linux recovery acceptance envelope.

The scan reads each log block at most once, including descriptor payloads. A
complete circuit without an independently recognized end is rejected instead
of revisiting the starting block. A payload that would cross that bound is also
rejected. Transaction count is bounded by the ring, eliminating half-range
ambiguity within the committed window. An end marker itself can occupy the last
unvisited ring block.

Memory is two journal blocks plus `A * sizeof(ReplayAction)` and
`R * sizeof(Revocation)` bytes, with `A` and `R` chosen by the caller. Capacity
exhaustion returns `no_memory` without a plan, including when the pending tail
would have been discarded. Input is scanned incrementally; the implementation
does not load an entire disk structure into memory. Revoke filtering is
`O(A * R)` and intentionally bounded; production recovery needs appropriate
indexes/passes and reserved memory, not a claim that this model scales to
workstation volumes. Existing scratch is not protected by concurrency locks.

## Evidence and reproduction

Run from the Kernel repository:

```sh
python3 scripts/test_jbd2.py
ASAN_OPTIONS=detect_leaks=0 python3 scripts/test_jbd2.py --sanitize
```

The hosted runner requires a C++23 compiler and the e2fsprogs `libext2fs` shared
library. `generate_jbd2_fixture.py --check` reconstructs the fixed synthetic
fixture with **e2fsprogs' independent `ext2fs_crc32c_le` implementation**, not the
production header's CRC routine. The oracle's `123456789` raw-CRC vector is
`0x1cf96d7c`. Fixture generation used e2fsprogs 1.47.0. The C++ corruption tests
also use that library to reseal structurally invalid inputs, ensuring failures
are not only checksum failures.

`tests/fixtures/jbd2_v3.bin` contains a superblock plus nine records packed in
traversal order. Its journal ring is `[1008,1024)`, starting at 1022. The first
transaction (`0xffffffff`) writes targets 40 and 41; the second (`0`) revokes 40
and writes 42. Target 41 uses ESCAPE. A valid old commit terminates the log.
The expected surviving actions are target 41 at journal block 1008 and target
42 at block 1012. This synthetic geometry exercises wrap with compact test
storage; it is not a Linux-generated filesystem image or a crash experiment.

Coverage includes all three admitted block sizes, 64-bit targets/revoke entries,
commit/checksum corruption, truncated records, malformed tag UUIDs/flags/tails,
missing final tags, misaligned revoke counts, unsupported feature combinations,
UUID/target/journal bounds, invalid domain overflow, escape misuse, future and
ambiguous sequences, capacity/read failures, full-ring rejection, uncommitted
revokes, same-transaction revokes, later reuse, repeated targets, multiple
descriptors, and deterministic planning without input mutation.

Source semantics are frozen to the locally available Linux **6.13.4** tree.
The implementation follows the structures and format semantics, not Linux's
execution environment. Source SHA256 values are recorded in
[`jbd2-sources.json`](jbd2-sources.json). Key references:

- `include/linux/jbd2.h`: wire structures, feature/flag definitions, minimum log
  size, sequence comparisons and raw CRC-state helper.
- `fs/jbd2/journal.c`: `jbd2_superblock_csum`,
  `jbd2_descriptor_block_csum_set`, `journal_tag_bytes`.
- `fs/jbd2/commit.c`: `jbd2_commit_block_csum_set`,
  `jbd2_block_tag_csum_set`, descriptor UUID and flag emission.
- `fs/jbd2/recovery.c`: checksum coverage, payload escape restoration,
  scan/revoke/replay passes, and revoke record widths.
- `fs/jbd2/revoke.c`: `jbd2_journal_test_revoke` and latest-revoke semantics.

No native architecture coverage is inferred from these host tests. Remaining
acceptance includes ext4 feature/mapping/domain admission, coherent immutable
I/O, a replay executor with write ordering and crash-safe checkpoints,
idempotent interrupted recovery, orphan cleanup, recovery-on-read-only policy,
Linux/e2fsck interoperability, and native crash/reboot tests before publishing a
root. All ten architecture profiles and the OS's four userland profiles remain
unchanged; no xmake configuration or OS source consumption is introduced here.
