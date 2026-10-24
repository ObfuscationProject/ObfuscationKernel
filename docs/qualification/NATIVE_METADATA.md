# Native checked file metadata

UAPI 0.12.0 and native module ABI 70 expose six native metadata calls:

| Call | Number | Arguments |
| --- | --- | --- |
| chmod | 90 | path, mode |
| fchmod | 91 | fd, mode |
| chown | 92 | path, uid, gid |
| fchown | 93 | fd, uid, gid |
| fchownat | 260 | dirfd, path, uid, gid, flags |
| fchmodat | 268 | dirfd, path, mode, flags |

The native interface requires complete unsigned 32-bit mode, uid, gid and flags
values before narrowing. An fd accepts canonical zero-extended signed 32-bit bits
or native sign extension; arbitrary upper bits return EBADF. Unused argument
words must be zero. Native okcrt wrappers use syscall6 to supply those zeroes.
These are native numbers on every supported architecture, independent of the
architecture-specific Linux syscall ABI.

Paths use the existing checked user-memory copier and a 256-byte fixed buffer.
Null pointers return EFAULT, overlong copies return ENAMETOOLONG, and empty paths
require AT_EMPTY_PATH. The at variants accept AT_EMPTY_PATH and
AT_SYMLINK_NOFOLLOW only. Descriptor and path resolution retain the checked POSIX
service's ownership and cleanup rules; the runtime never calls legacy unchecked
chmod/chown helpers.

The provider returns ModeChangeResult. Applied changes return zero, read-only
providers return EROFS, unavailable capability returns EOPNOTSUPP, and failures
preserve their errno mapping. The ownership service and provider both require
effective uid zero, including an all-unchanged request. UINT32_MAX independently
preserves the existing uid or gid. A mode change retains the checked service's
owner/root and filesystem policy. Node gains a checked ownership virtual method,
so ABI 69 modules must not be loaded with ABI 70.

The SDK profile checked-mode-root-owner-u32-v1 identifies this software boundary.
The producer and consumer require the exact ABI, UAPI version, profile header,
scalar definitions and hashed dependency headers. Admission is not a statement
that every mounted provider supports ownership changes.

scripts/test_native_metadata.py builds the actual dispatcher helper and errno
mapper with xmake. It executes hosted 32-bit and 64-bit word models, injected
checked provider results and modeled user-memory copies. It does not execute a
native trap, prove provider persistence or qualify hardware. SDK contract tests
separately reject rehashed declaration/macro decoys and incompatible cohorts.
