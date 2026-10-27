# ABI64 fixed-profile common opening budget

This private arithmetic helper describes G4/F32768 with 1 KiB ext4 blocks,
128 checked non-root allocated inodes, an eight-member orphan chain and the
eight-family policy set. It includes the locator once, selected writer,
terminal validation, final journal admission and root decoder. It must be
rederived if any of these source bounds or accepted profiles change.

The independent source calculator is `sdk53-mounted-common-startup-union-budget-
advice-v1/advisory-attempt-01/calculate_v1.py`; its sealed result is
`bb734ef51551346282a43261db539160f6ee80e3fa691c9503f685b7901ab9df`.
The inherited CALCULATION.json records every primitive, all seven routes and the reclaim
remaining-member rows. The test oracle is generated from this pinned result,
independently of the C++ table. The largest ordinary route supplies 58,811,342,498
READs; the empty-orphan route supplies 120 WRITEs and 64 FLUSHes. Both attempted
and completed fields independently require the complete componentwise union.

`reserve` checks only arithmetic operands. `narrow` requires original attempted/
completed parity and exact four/five-read classifier progress, rejects any
classifier write/flush, and subtracts the classifier from the selected route.
The original baseline is used for reservation; the touched endpoint receives
only the remaining route allowance. Sequence deltas remain inert upper bounds;
real journal lower/upper bounds require authenticated native head selection.

## Successor derivation

The ABI64 source calculator is
`sdk62-journalled-mkdir-v1/common-opening-budget-derivation-v2/calculate_v1.py`.
It reproduces the pinned inherited calculator and raw CALCULATION.json before
deriving the successor. It retains all seven original rows as evidence, records
the current source SHA256 identities and emits the independent successor oracle.
It performs only checked integer arithmetic and read-only source inspection.
No compiler, hosted model, backend or guest is called by this calculation.

The calculator SHA256 is
`f3b524c03b6f0238cdb7863359a483f443d652b41352e60d8213983474506258`.
The successor CALCULATION.json SHA256 is
`f28f9a0ff006bfd620f6e9641b951e014b991e3efe85200e4b2397c9fcf206a3`.

| Family | Inherited READ bound | ABI64 READ bound | WRITEs | FLUSHes | Sequence delta | Locator READs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ordinary | 58,811,342,498 | 58,811,342,498 | 8 | 6 | 2 | 4 |
| legacy | 23,524,424,765 | 23,524,424,765 | 7 | 5 | 2 | 5 |
| empty orphan | 11,762,325,977 | 11,762,325,977 | 120 | 64 | 24 | 4 |
| namespace recovery | 11,762,191,265 | 11,762,191,265 | 21 | 11 | 5 | 5 |
| reclaim | 23,524,315,345 | 23,524,315,345 | 111 | 61 | 23 | 5 |
| typed zero | 2,917,720,019 | 2,917,720,020 | 5 | 3 | 1 | 5 |
| typed allocated | 2,917,720,034 | 2,917,720,035 | 5 | 3 | 1 | 5 |
| directory create | absent | 23,524,132,523 | 9 | 5 | 2 | 5 |

The typed rows grow by exactly one attempted/completed READ. In
`ext4/detail/owned_typed_port.inc`, admission reserves `child_reads + BufferCapacity`,
sets `prefix_reservation_ = BufferCapacity` and rereads each ready resident slot
in a loop bounded by BufferCapacity. The mounted owner now has nine slots.
Comparison loops that merely inspect in-memory slots add no READs. The bound
does not rely on a cold classifier leaving the cache empty.

The new directory family is appended; prior family enum values retain their
identity. Only the checksum-validated `OKDRW001` marker and matching seven/eight
descriptor tags select this family. A semantic witness failure is terminal and
cannot fall through to the legacy row or a regular-file replay proof.

For the new row, let G=4 groups, N=128 checked allocated slots, I=18 maximum
READs per inode decode, V=1,062 per checked view open, U=2,327 per unique strict
directory mapping and A=8 home actions. Every logical read is charged as a
physical READ, including reads served by retained before/after overlays.

- Old free-block ownership scan S = 1 + G + N*(1+I) = 2,437. The first read is
  the selected block bitmap; the remaining reads include every inspected raw
  table slot and checked decode, including reserved allocated inode handling.
- A lookup after strict one-block mapping proof costs L = 2*I + 1 = 37.
- Witness semantics excluding its old view open costs
  2*A + 1 + S + 3*U + 3*L = 9,546. These are the home payload/raw reads, witnessed
  descriptor, old free-block scan, old/final parent and new directory mappings,
  and final destination/dot/dotdot lookups. Codec/planner/reencoding are pure.
- Preparation costs 12 binding + 1 journal head + 32,767 parser + 2*V
  final/old views + 9,546 semantics + 11,761,969,210 allocated validation
  + 2 prepared head/super reads = 11,762,013,662.
- The whole route costs 5 locator + 11,762,013,662 preparation + 2 original
  head/descriptor bookend + 8 execution payloads + 11,762,118,846 inherited
  terminal/root continuation = 23,524,132,523 READs.

The nine WRITEs are eight homes and journal cleanup. The five FLUSHes are the
initial flush, table barrier, allocation/new-data barrier, home flush and journal
cleanup flush. Dirty filesystem state is retained; there is no extra clean
superblock write. The cleanup sequence advances by two and the locator costs
five READs. The componentwise six-counter union remains 58,811,342,498 READs,
120 WRITEs and 64 FLUSHes; all ten native ABI-width rows remain in the evidence.

No arithmetic result authenticates an opening, family, Session, child or root.
The retained native common locator, guarded endpoint adoption, selected child,
renewal and publication require separate execution evidence. This arithmetic
receipt does not export an SDK or qualify guest execution.

## Distinct regular-move successor

The appended `regular_move` row preserves every earlier family value and every
older bound. Its active marker is `OKMVW001`; it never chooses namespace or
legacy fallback. The fixed profile still has four groups, 32,768 filesystem
blocks, 1 KiB homes and at most five changed homes.

The source-derived cold completion budget is 143 READs: twelve generic binding
reads, at most twenty-five retained physical observations, twenty-five initial
bookends, at most six canonical misses, twenty-five execution bookends,
twenty-five physical verification reads and twenty-five terminal bookends.
Duplicate physical homes can lower this bound. The strict planner, witness
reencoding and sealed journal parsing perform no additional physical reads.
The completed family independently proves an empty orphan chain; it schedules
no orphan drain. There are at most five missing home writes and one empty-head
write, and exactly three completion flushes. Cleanup advances the active
sequence by two.

The original address-bound cold cursor continues to guard the existing strict
ordinary decoder before terminal root admission. Add the retained terminal/root
continuation bound 11,762,118,846, the five classifier reads and one final head
read: the whole family allowance is 11,762,118,995 READs, six WRITEs, three
FLUSHes, sequence delta two and five locator READs. Both attempted and completed
fields are reserved before family execution; actual service calls and canonical
cache misses adopt only their physically verified Session prefixes. The global
componentwise union is unchanged. These arithmetic bounds are not execution,
recovery or root-publication authority.

## ABI70 typed cursor reservation alignment

The pure mounted typed budget now derives its Owner prefix reservation from
`ext4_directory_create_owner_buffers`, the same nine-slot constant used by the
production mount. Its earlier eight-READ inner reservation could reject a
completed child after nested directory creation filled all nine canonical cache
slots. The actual Owner already reserved and verified all nine original prefix
READs; the cursor correctly refused the inconsistent smaller allowance.

The five pure mutation route totals and the two pure startup totals each grow
by one READ. The typed startup totals become 2,917,720,020 and 2,917,720,035.
These agree with the existing ABI64 common-opening table above, which already
included the ninth slot. The common-opening rows and union do not grow again.
All original child, prefix, identity, lifetime and counter comparisons remain
in place. The account split-write model preserves the nested-directory history
and checks both root and UID/GID 1000; hosted execution and native evidence are
recorded separately.
