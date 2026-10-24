# Checked Linux mode changes (isolated prototype)

This source implements a bounded Linux chmod family over the common POSIX service
and a checked runtime filesystem boundary. It is an unexported prototype after the
frozen SDK21 candidate. Its changed Node vtable and POSIX methods require a new C++
SDK generation before any producer/OS integration; it must not be exported as
SDK21 or substituted into an SDK21 artifact. The native C UAPI is unchanged.

## Behavior and ownership

`ChmodCallAdapter<Profile>` decodes chmod (where the target provides it), fchmod,
three-argument fchmodat and four-argument fchmodat2. Generated numbers and errno
values crosscheck pinned Linux 6.13.4 and musl 1.2.5 inputs on all ten profiles.
Mode arguments retain only 07777; fd and flags use Linux signed/unsigned32 widths.
Unknown flags fail before path copying. NULL paths always fail EFAULT, including
AT_EMPTY_PATH; an empty string without AT_EMPTY_PATH fails ENOENT. The old
fchmodat has no fourth argument. These calls use at most four saved registers,
including MIPS O32. EOPNOTSUPP completes as122 on MIPS/MIPS64 and95 elsewhere;
EROFS is30. Global Status-to-errno conversion is unchanged.

`PosixService::change_mode_at` resolves checked paths with directory search access
and final follow/no-follow semantics. Absolute paths ignore dirfd. Empty-path fd
or cwd operations address the retained object after rename/unlink/detachment;
no pathname reconstruction or new search check is introduced. A live node-less
console/pipe description is unsupported; invalid/closed descriptors fail EBADF.
Mode changes preserve offset, open flags, descriptor flags and node identity.
Temporary path references are released on all outcomes. A provider error wins
over cleanup failure; cleanup failure after successful commit is reported.
Unsupported resolver metadata is an operation-local unavailable result.

`Node::change_mode` refuses by default. `Ext4Node` admits explicitly maintained
writable mounts, regular files/directories, supported inode metadata and checked
write eligibility before requesting one timestamp event. Readonly mounts return
EROFS before owner/type checks. Other providers, unqualified writable mounts,
symlink mode changes and unsupported inode metadata return EOPNOTSUPP. Authority
uses effective UID/GID: root or owner may change mode; nonowners receive EPERM;
nonroot owners changing a foreign-group inode have SGID cleared. Real IDs do not
grant authority. Requested SUID/sticky bits are stored; this makes no claim about
set-ID execution. Mode and ctime are staged from one admitted inode record, even
for same-mode requests. atime, mtime, ownership and inode type are preserved.
Missing qualified clocks fail EIO; write failures follow existing quarantine.

The existing internal chmod operation retains useful RAM/filesystem behavior and
shares the pure owner/SGID policy. It is not a fallback for checked public calls.

## Evidence and limits

Hosted suites exercise the real VFS/POSIX/provider composition, raw image mode
and ctime changes, readonly/unqualified and permission refusals, faulty cleanup,
held identity, unsupported large inode metadata and injected write failures.
Separate actual-dispatcher tests use modeled user memory and injected providers;
they cover all ten register profiles, copy ordering and error completion. These
are distinct from target object compilation and actual user instructions.

```sh
python3 scripts/test_linux_stat_blocks_oracle.py --output /tmp/chmod-oracle
python3 scripts/test_linux_chmod.py --output /tmp/chmod-dispatch
python3 scripts/test_linux_chmod_provider.py --oracle /tmp/chmod-oracle --output /tmp/chmod-provider
python3 scripts/test_linux_chmod_service.py --oracle /tmp/chmod-oracle --output /tmp/chmod-service
```

Each runner requires a fresh output, retains its source/build inputs and reports
scope. The suites also accept `--sanitize`. Existing initial-child and checked-VFS
regressions run with the service suite; prior maintained ext4 timestamp/fault
tests remain separate. OS owns the native fixture launcher, musl public API probe,
image/oracle tools and distribution policy. Building the musl probe does not
constitute executing it.

Calls currently require the serialized boot CPU context. Retention protects
lifetime but does not establish concurrent permission/mutation atomicity. No
supplementary groups, fsuid/capabilities, set-ID exec, SMP, general POSIX Issue8,
crash atomicity, full ext4 or physical-platform support is claimed. Private SDK
integration, standalone native boot, actual root/nonroot musl guests, Linux
reference interoperability, independent review and full Debug/Release producer
and OS artifact-consumer qualification remain open.
