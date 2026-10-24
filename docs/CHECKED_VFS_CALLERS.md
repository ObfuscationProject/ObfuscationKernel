# Checked filesystem callers candidate

This staged follow-up migrates POSIX descriptor/path callers, native executable
loading and fallible VFS operations onto the ABI 9 query_metadata/lookup_child
services. It does not change virtual slots, enable a filesystem format, add
concurrency or qualify shared main-tree source.

Metadata errors propagate through fstat, SEEK_END, append, readiness, directory
fd resolution, getdents, cwd, chmod/stat/readlink and native exec admission.
SEEK_SET/CUR do not read metadata. Append keeps the old shared offset if metadata
or the write fails. Namespace mutation checks distinguish a missing destination
from an unreadable one before invoking the filesystem mutator.

Successful pathname operations explicitly finish their temporary pin. Cleanup
consumes its pin even on failure; original operation errors keep priority. chdir
finishes temporary path cleanup before publishing the new retained cwd. Replacing
an old cwd consumes its old pin after installing the new owner; a cleanup error
is returned with the new cwd still valid. Native load completes executable pin
cleanup before constructing/publishing the candidate address space, reclaiming
all unpublished image pages on failure.

Legacy pointer-only lookup remains for callers whose API cannot return an error;
RAM-only archive internals retain infallible metadata/lookup. In-filesystem ext4
helper migration, concurrent locking/lifetime semantics and complete POSIX remain
outside this candidate. No all-ten integrated qualification is claimed.

Validation so far: preserved all-ten path ownership/process-resource models,
ext4 injected cleanup tests, kernel service models and Linux runtime sanitizer
pass. New checked-call tests pass all ten hosted profiles and ASan/UBSan; the
shared ABI9 negative control fails on the first append metadata assertion.
Executable read/metadata/cleanup faults and recovery pass ASan/UBSan; baseline
negative control fails the expected exec metadata assertion. Three isolated Debug standalone Kernel/SDK-consumer pilots (x86_64, i386, MIPS)
passed all 45 commands, including native and musl file/heap execution, persistent
ext4 operations and device-derived journal replay. Exact retained SDKs and source
archives are under /tmp/obfuscation-vfs-checked-os/build/checked-vfs-pilots.

Independent review additionally found and corrected hard-link destination
admission: VFS now performs checked lookup before the provider's link mutator,
propagates an unreadable target, and rejects an existing target. The focused
regression fails on the prior candidate and passes after the guard; all ten
hosted profiles and the preserved RAM namespace suite pass. The 45 native pilot
commands above qualify their frozen pre-review snapshot; final integrated native
qualification must include this guard.
