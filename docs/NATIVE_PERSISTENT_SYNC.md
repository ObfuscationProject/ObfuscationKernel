# Strict persistent fd synchronization

Native UAPI 0.9 adds `OK_SYS_SYNC_PERSISTENT` (1031), version
`OK_NATIVE_PERSISTENT_SYNC_VERSION == 1`. Argument zero is a nonnegative fd
representable by `int32_t`; arguments one through five must be zero. Success is
zero. Invalid descriptors return EBADF, nonzero reserved arguments EINVAL, and
volatile or unqualified providers EOPNOTSUPP. Linux personalities have no mapping
for this native extension. Ordinary fsync/fdatasync retain their existing scope.

`PosixService::sync_persistent` dispatches through the actual descriptor's retained
Node. The default Node implementation refuses, including RAM/archive files and
directories. Console, pipe, socket and volatile shared-file descriptors cannot
admit persistent storage. No filename, file contents or exported capability marker
can turn a volatile fd into an accepted persistent provider.

The mounted ext4 implementation admits only `ordered_data_dirty_noatime`. It checks
the original Node slot, mount/session, inode generation and linked lifetime, plus
an actual address-bound descriptor lease. It holds an inode I/O lease throughout
checked physical-generation reads and the original owner's Session/device FLUSH.
The mount's serialized reader admission blocks mutations and reentrant calls.
Completed ordinary mutations already finish the bounded journal/checkpoint
protocol; the additional FLUSH is still mandatory. Both regular-file and directory
fds are supported, including read-only fds. Detached, final, stale, unretained and
foreign identities are rejected. Transport or authority failures quarantine the
provider and release the admitted I/O lease. The final authority/lease check must
succeed before returning success. A normal sync keeps `needs_recovery` set while
the filesystem remains mounted; clean shutdown uses the separate drain/freeze
boundary.

The exported manifest requires
`native_persistent_sync_profile = "retained-linked-mounted-ordered-journal-device-flush-v1"`
and the canonical `ok/fs/persistent_sync_profile.hpp` declaration. The Node
virtual operation introduced with ABI68 changes the exported C++ vtable. The
current shutdown development cohort is ABI69 and requires matching headers and
rebuilt consumers; ABI68 and ABI67 binaries are incompatible. Native UAPI0.10.0
also provides the separate original-init scalar power PREPARE/queued-COMMIT contract.
All ten architecture profiles retain the same native syscall number and fd
contract. Production mounted storage retains the one-device/nine-buffer owner;
no cache capacity or optional-revoke pairing is weakened by this operation.

## Verification and limits

`tests/kernel/points/persistent_sync_checks.hpp` is reached by the Debug kernel's
test-point runner. It checks RAM, bad-fd and reserved-argument refusals only.

The hosted runner exercises the actual mounted provider, POSIX service, generic
syscall Table and native dispatcher over the model block device. It checks file
and directory FLUSH, journaled file mutation, I/O-lease lifetime and reentry,
transport-error quarantine, native/Linux separation, and all five reserved
arguments. Optional `--private` explicitly disables access control only in the
hosted fixture to attack copied, detached, unretained, aborted and foreign
identities. Sanitizers do not turn this into guest or physical-durability evidence.

The fixture must be selected explicitly. For the retained Linux-journal fixture:

```sh
python3 scripts/test_persistent_sync.py \
  --fixture ../ObfuscationOS/build/jbd2-linux-fixture-v1 \
  --output ../build/qualification/persistent-sync-host-new \
  --sanitize --private
python3 scripts/test_persistent_sync_cross.py \
  --toolchain-root toolchains \
  --output ../build/qualification/persistent-sync-cross-new
```

The hosted runner revalidates the captured image hash, independent admission and
exact fixture header before applying the existing clean-capture oracle. Default
mke2fs output is not a substitute for the admitted activated JBD2 fixture. The
capture records Linux source provenance as not established. The cross runner
compiles the production provider specialization and dispatch paths for all ten
profiles; those objects are compile evidence only. Standalone Debug QEMU boot,
SDK export/OS-consumer qualification, native guest execution and physical
power-loss durability remain separate evidence layers.
