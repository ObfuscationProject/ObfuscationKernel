# Ordered directory initialization mutation budget

These source bounds reserve an owner mutation and its mounted continuation.
They do not authenticate a Session, create authority, qualify a model, load
code or establish native or hardware acceptance. The continuing Owner must
retain and observe the same original binding, baseline and phase endpoints.
Session counters are u64 on all ten architecture profiles, including those
with 32-bit pointers. All twenty architecture/mode rows and four supplied
musl profiles remain in scope.

The accepted directory profile has 1 KiB blocks, at most G=32 groups,
8,192 blocks/group, F=262,145 filesystem blocks and 8,192 inodes/group.
N=128 allocated ownership slots include reserved allocated inodes. A=8 homes
and H=K=9 participants/buffers cover distinct parent/new inode tables; the
shared-table case uses seven homes. Directory scans admit B=1,024 blocks
and at most C=84 occupied callbacks/block. A live record needs at least twelve
bytes in the 1,012-byte directory body.

The initial directory-only format probe costs one READ and refuses other
ext4 block sizes before the initial clean Completion. Its retained format and
geometry must agree with the actual clean preparation; the mutation bounds
must not be applied to a decoder of an unadmitted 2 KiB or 4 KiB filesystem.
The internal journal inode admits one contiguous extent of at most 32,768
blocks. JBD2 read_next stops before revisiting its first block, so its parser
bound is J=32,767 independently of the larger filesystem bound.

Each logical read is conservatively charged as one physical Session READ,
including reads an old/final overlay or resident cache may satisfy in memory.
Report, geometry, binding and lock-only comparisons add no transport READ.

| Primitive | Formula | G32 bound |
| --- | --- | ---: |
| Inode decode I | 2 + 4G | 130 |
| Checked view V | 2 + 2I + B | 1,286 |
| Unique directory mapping U | I + 1 + G + NI | 16,803 |
| No-reference scan S | 1 + G + N(1 + I) | 16,801 |
| Strict one-block lookup L | 2I + 1 | 261 |
| Generic lookup Q | 2I + B | 1,284 |
| Checked directory D | 3I + B + BC(2I + B) | 110,445,958 |
| Allocated inspection GA | D + G + N(I + D) | 14,247,545,254 |
| Binding T | 4 + 2G | 68 |
| Empty Completion E | T + 3 + V + GA | 14,247,546,611 |
| Initial refusal ER | T + 1 + J + V + A(2 + 4I) + GA + 2 | 14,247,583,554 |
| Directory witness W | 2A + 1 + S + 3U + 3L | 68,010 |
| Strict proposed/checkpoint P | T + 1 + J + 2V + W + GA + 2 | 14,247,648,674 |
| Stored metadata ST | G + (1 + G * 8,192)I | 34,078,882 |

D includes a second parent decode on the rejecting duplicate-dotdot prefix.
GA charges all 128 checked ordinary slots and the root separately. ST has no
local 128 gate, so it charges every possible allocated slot and the root.
The initial structural Completion can parse and inspect a nonempty log before
rejecting its shape; ER charges that refusing prefix and eight table actions.
The pure planner, witness codec and canonical reencoding perform no I/O.

| Owner/continuation stage | Source operations | READ reservation |
| --- | --- | ---: |
| Nonzero read-only preparation | Probe1 + ER + Q + U + 2G + 6 + S + K pin misses | 14,247,618,522 |
| Already-zero read-only preparation | Nonzero preparation base + immediate strict proposed P | 28,495,267,196 |
| Public preparation cap | Maximum of the mutually exclusive preparation branches | 28,495,267,196 |
| Before zero WRITE | Frozen head/all-home comparison H, ownership S, second physical bookend H | 16,819 |
| After zero FLUSH | Explicit candidate readback1, frozen head/all-home comparison H, ownership S | 16,811 |
| Deferred strict proposed replay | P, including its own old-view ownership proof | 14,247,648,674 |
| Before journal publication | Final physical head/all-home bookend H | 9 |
| Nonzero initialization stage | 2S + 4H + 1 + deferred P | 14,247,682,313 |
| Already-zero initialization stage | Ownership S and three physical bookends H | 16,828 |
| Public initialization cap | Maximum of the mutually exclusive initialization branches | 14,247,682,313 |
| Checkpoint | Published descriptor/head/payload/commit/end h+4, strict P, h payload reads | 14,247,648,694 |
| Mounted continuation | Conservative prefix I+Q, post-checkpoint H, ER, V, ST, 3L, 3I | 14,281,666,318 |
| Whole nonzero mounted branch | Nonzero preparation + nonzero initialization + checkpoint + continuation | 57,024,615,847 |
| Whole already-zero mounted branch | Already-zero preparation + already-zero initialization + checkpoint + continuation | 57,024,599,036 |
| Whole mounted reservation | Maximum complete branch, charging proposed P once | 57,024,615,847 |

The already-zero branch retains immediate strict replay preparation and performs
no initialization write or flush. The nonzero branch defers that replay until
the zero write, flush, readback and ownership proof succeed. Public phase caps
therefore describe different maximum branches: their sum is not the whole
reservation. The Owner also checks cumulative actual deltas against the whole
cap at every phase endpoint, so unused preparation allowance cannot enlarge a
later whole-operation allowance.

The whole reservation includes the pre-Owner mounted prefix even if it has
already completed before the real Owner baseline. That is conservative
headroom; the continuation endpoint adopts only actual measured progress.
The post-checkpoint H direct reads compare every participant against its
canonical cache image. Those participants remain resident, so the paired
canonical pins need no additional physical miss. The three lookups are the
created name, dot and dotdot. The three inode decodes are created metadata,
changed-parent metadata and final created-inode lifetime validation.

On the nonzero branch, preparation, the repeated scan before zeroing and the
repeated scan after the data flush are three explicit ownership passes. Strict proposed replay
and strict checkpoint each reconstruct the old allocation view and include
another S. All five proofs are accounted; a counter reservation never
substitutes for any proof. The extra pre-zero bookend and explicit physical
candidate readback are counted separately.

Publication writes descriptor1 + h payloads + end1 + commit1 + head1, thus
h+4 WRITEs and four FLUSHes. Normal mounted dirty checkpoint writes h homes
and the empty journal head, thus h+1 WRITEs and five FLUSHes. It retains dirty
filesystem state. Nonzero initialization adds exactly one zero WRITE and one
data FLUSH, giving 2h+6 WRITEs (20 or 22) and ten FLUSHes. The already-zero path
adds neither, giving 2h+5 WRITEs (19 or 21) and nine FLUSHes. Do not fold the
data flush into an existing journal barrier. Cleanup advances sequence by two.

Reserve all six attempted/completed counters independently before preparation.
Require healthy owned state, original nonzero identity/registration/session,
no in-flight request, no pending writes and completed parity. Capacity checks
use subtraction through session_counter_fits, so narrow finite policies
refuse before wrap. The default remains u64 on every architecture. Endpoint
progress comes from actual original Session observations; never advance a
shadow by its reserved maximum. Initialization write/flush failures, partial
or after-effect writes and every later logical mismatch quarantine the Owner.

Before an admitted zero WRITE, initialization deltas (W,CW,F,CF) are all zero.
An admitted failed WRITE leaves (1,0,0,0) and pending writes. A successful WRITE
before flush leaves (1,1,0,0). An admitted failed FLUSH leaves (1,1,1,0) and
pending writes. Successful FLUSH leaves (1,1,1,1), with pending writes cleared;
failed readback or proof still quarantines without journal publication.
Session completion credit means transport plus retained-authority success,
not a hardware persistence qualification.

The existing Common G4/F32768 cold directory row is unchanged: 23,524,132,523
READs, nine WRITEs and five FLUSHes. Cold replay continues to require the exact
strict zero-preimage witness. Live initialization allowances never broaden
that cold route. The independent arithmetic model checks the G32 constants,
source stage counts, general G1..G32 monotonic bounds, independent six-field
counter edges, narrow policies and the retained Common cold row. Execution
of that model belongs to separate backend qualification.
