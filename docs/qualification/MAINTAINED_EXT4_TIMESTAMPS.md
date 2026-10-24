# Explicit maintained ext4 timestamps

Status: SDK20 bounded integration passed six-layout hosted/sanitized fault tests,
all-ten Debug/Release producer/consumer gates, actual native/musl UID1000 fixtures,
Linux readonly interoperability and a fresh shared x86 rebuild. Native UAPI0.6.0
and its stat record are unchanged. Linux stat-family dispatch remains unsupported.

## Admission and lifetime

`Ext4FileSystem::mount(device, Ext4MountOptions{true,
Ext4TimestampPolicy::maintained})` explicitly selects maintained A/M/C for the
existing clean, nonjournalled ext4 subset. It requires a checked active realtime
sample, sound stored root timestamps and writable supported root format. The
sample must fit every root field before the root is published. Admission writes
no timestamp. Failure does not fall back to an unqualified mount.

The existing `mount(device, bool writable=true)` remains unqualified, including
after later clock setting. Distribution root adoption is a separate policy gate.
The trusted mount owner accepts the image's existing stored times as its
baseline; a clean bit or a clean remount does not repair unknown past history.

Successful maintained checked metadata supplies all three stored fields with
`TimestampSource::maintained_ext4`. This denotes provider event maintenance,
not physical RTC origin, UTC accuracy, or persistent clock generation. The
operation record retains its full actual realtime sample locally; ext4 does not
persist that origin or generation. Readonly mounts retain `readonly_ext4`.
Default writable mounts, RAM, archive-v1 and synthetic providers keep their
existing unqualified timestamp behavior.

Provider, block device and VFS identity registration must outlive every node and
retained description. Calls use the existing serialized boot-CPU ownership
contract. Clock callers may not run on another CPU or in IRQ/NMI context. This
change adds no unmount, block ownership, general locking or SMP guarantee.

## Provider event policy

Every admitted event takes one checked `arch::realtime_sample()` after ordinary
validation and known capacity checks. All affected local inode records must
encode before the first disk write. Multi-inode operations share the sample.
Refreshed structural inode records receive the same planned fields, preserving
new extents and link counts. Large user requests may invoke multiple provider
events; this is not a one-sample-per-syscall promise.

| Provider operation | Updated fields |
| --- | --- |
| Create, mkdir, symlink | Child A/M/C; parent M/C |
| Nonempty write, including identical overwrite and sparse growth | File M/C |
| Explicit truncate, including same size and zero size | File M/C |
| Link | Source C; destination parent M/C |
| Unlink/rmdir | Retained victim C; parent M/C |
| Rename/move | Source C; old/new parents M/C; retained replacement C |
| Explicit chmod/chown, including equal values | Target C |
| Nonzero regular read, including EOF | File A |
| Nonzero symlink read/follow | Link A |
| Access-intent directory listing, including empty/EOF | Directory A |

Same-inode rename, zero-count read/write, metadata queries, lookup, inspection
listing, topology checks, sync, close and final orphan reclamation take no event
sample. Directory `..` changes do not invent an A/M event for the moved directory.
Internal emptiness checks use `DirectoryReadIntent::inspection`; ordinary callers
default to `access`. Enumerated children's stored times are unchanged.

This mode implements strict access updates. It has no relatime, lazytime,
noatime or O_NOATIME option. Read permission authorizes an automatic atime update;
it does not require clock-setting root authority. VFS/POSIX permission and path
admission remain separate. A completed symlink access can precede a later failed
path operation and keeps its atime.

Reads and listings acquire bounded private data, commit required atime, then
return that value to their consumer. Clock/encoding/save failure returns no
provider buffer or listing. The existing POSIX output/offset boundary therefore
does not publish data from a failed provider call. A later consumer, path-release
or user-copy error may leave an already completed atime; no rollback is claimed.

## Storage format and errors

The format-only `encode_ext4_times` validates the nonempty field mask, normalized
nanoseconds, inode width and each declared extra-field extent before changing any
byte. Selected fields without an extra word require signed32 seconds and store
zero nanoseconds. Extended fields support seconds from -2147483648 through
15032385535 and use the ordinary little-endian ext4 low32/epoch-bit encoding.

New fully zeroed 256-byte maintained inodes declare exactly `i_extra_isize=16`,
covering A/M/C. New 128-byte inodes store seconds precision. Existing declarations
never grow; unselected and unknown bytes are preserved. Birthtime, explicit
user timestamp setters and ext4 orphan-link reuse are outside this change.
The current administrative setter admits a narrower domain than the disk codec.

Unavailable/invalid active clocks return EIO without poisoning healthy storage;
unrepresentable selected storage times return EOVERFLOW before effects. Sound
stored metadata remains readable if only the clock is unavailable. Malformed
stored times and transport failures are storage errors. Unsupported inode
mutation features must be refused before sampling or writing.

This differs deliberately from Linux old-field clamping and best-effort atime
error handling. Successful common-domain encodings must interoperate with Linux;
the stricter refusal behavior is not a claim of identical Linux semantics.

## Failure and evidence boundaries

Success includes every required data, namespace and timestamp save. Flush
durability remains the separate existing sync contract. Writes are ordered and
nonjournalled; they are not atomic across power loss. A creation inode has its
initial attributes and A/M/C before its first parent-entry publication.

An uncertain effect quarantines the maintained mount. Future writes, sync and
checked maintained metadata fail. A device may report error after zero, partial
or complete media effects; successful-write counters alone are insufficient.
Only proved prepublication allocation rollback may preserve writable admission;
possibly published allocations must not be freed as rollback. Clock failures
are separate and may recover on the same mount without a storage reset.

Hosted models link an explicit test-only realtime provider with observable sample
count, unavailable by default. Production links the architecture service directly.
Hosted image/fault models, target-object builds, actual SDK-only Virtio diagnostics,
actual native/musl execution and readonly Linux mounts are separate gates. Keep
their exact source generations and initial/final image identities distinct.

`debugfs` 1.47.0 only displays extended timestamps when extra-isize is at least24;
raw per-field interpretation and a Linux mount are required for the exact new16
choice. Offline e2fsck does not establish atime policy, power-loss durability,
physical RTC accuracy, SMP behavior, Linux stat support or complete ext4/POSIX.
