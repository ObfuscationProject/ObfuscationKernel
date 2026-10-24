# Access permission queries

`PosixService::access` and `faccessat` check the current DAC model. Real UID/GID
are the default. `AT_EACCESS` selects effective IDs for both pathname search and
the final inode check. Mode accepts only F_OK/R_OK/W_OK/X_OK; flags accept only
AT_EACCESS/AT_SYMLINK_NOFOLLOW/AT_EMPTY_PATH. Invalid bits fail with EINVAL.

Absolute paths ignore dirfd. Relative paths retain the existing unified resolver
behavior. An empty pathname requires AT_EMPTY_PATH and checks an open descriptor
or held cwd without reconstructing its pathname; a NULL user pointer still fails
with EFAULT. Directory traversal requires search permission even for F_OK. The
validity of a special pipe/socket/console descriptor supports an empty-path F_OK
existence check. R_OK/W_OK/X_OK on these descriptions return ENOSYS until their
owner and permission metadata are qualified; fixed fstat placeholders do not
participate in DAC decisions. Backed node descriptors retain ordinary mode queries.
The
selected owner class takes precedence over group and other mode bits. Root's
regular-file X_OK needs at least one execute mode bit; directory search retains
the current root DAC bypass.

The Linux adapter implements access where the pinned architecture table contains
it, three-argument faccessat and four-argument faccessat2 on all ten profiles.
Old faccessat has no flags register. Path bytes cross the checked user-copy
boundary once, up to the existing 255-character limit. No caller credential
mutation or retained callback participates in a query. A query grants no future
file-opening or write authority and does not eliminate access/open races.

This is not complete Linux or POSIX access conformance. The credential model has
one primary group, no supplementary groups, filesystem IDs or capabilities.
ACL, read-only/noexec mount and immutable-file policy are unqualified. W_OK
evaluates DAC bits only and does not promise provider write support. Synchronous
resolver contention may return EBUSY; a blocked access continuation and SMP
credential synchronization remain open. Hosted adapter/resolver checks are model
evidence; native user ELF/trap results must be recorded separately.
