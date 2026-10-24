# Native Process Credentials

The additive native credential capability is
`bootcpu-single-live-thread-real-effective-saved-v1` and
`OK_NATIVE_CREDENTIALS_VERSION` is 1. The calls were added in UAPI 0.7.0; the
current UAPI is 0.11.0 and the module ABI is 69. Consumers must require the capability in their exact
architecture/mode-specific SDK manifest before using these calls.

The native numbers are uniform across all ten Kernel architecture profiles:

| Call | Number | Arguments |
| --- | --- | --- |
| `setuid` | 105 | UID |
| `setgid` | 106 | GID |
| `setreuid` | 113 | real UID, effective UID |
| `setregid` | 114 | real GID, effective GID |
| `setresuid` | 117 | real UID, effective UID, saved UID |
| `getresuid` | 118 | real UID pointer, effective UID pointer, saved UID pointer |
| `setresgid` | 119 | real GID, effective GID, saved GID |
| `getresgid` | 120 | real GID pointer, effective GID pointer, saved GID pointer |

Each ID argument consumes its low 32 bits. `OK_ID_UNCHANGED` (`0xffffffff`)
preserves that component for `setre*` and `setres*`; it is invalid for the
single-ID setters. `getres*` writes three four-byte `uint32_t` values in target
byte order. Stores happen left to right, including when pointers alias. A later
invalid pointer returns `-OK_EFAULT` after preserving earlier successful
stores. No user pointer is retained.

The actual native syscall boundary authenticates the running caller and uses
checked user-memory copies. Native mutation reuses the same
`change_credentials` authorization as the Linux personality: all decisions use
the immutable old real/effective/saved tuple. An effective UID of zero permits
arbitrary user/group IDs. Unprivileged calls can select only the IDs permitted
by that operation. Effective GID zero does not confer UID privilege. No native
request can grant `kernel_space`, and attempts to mutate kernel credentials
fail with `-OK_EPERM`.

Single-ID setters by a privileged caller replace all three IDs in their domain.
Therefore a service can permanently drop GID before UID, then verify all six
IDs with `getres*`. Temporarily preserving saved UID zero permits a later
effective UID restoration; permanently replacing all UID components removes
that path. Changing UID before GID can remove the authority needed to change
GID.

Setters require exactly one non-exited TCB in the process. Zero live threads or
multiple live threads return `-OK_EOPNOTSUPP` without publishing a candidate.
Blocked threads count as live; exited slots do not. This is a serialized boot
CPU mechanism. Per-thread credentials, libc set-ID synchronization, hardware
SMP identity mutation, supplementary groups, capabilities, set-ID executable
bits, session/controlling-terminal setup and account authentication are separate
work.

`tests/host/native_credentials_tests.cpp` exercises the actual native runtime
dispatch with hosted trap/memory backends: caller identity, root/nonroot policy,
all eight forms, high IDs and argument truncation, permanent GID-before-UID
drop, scalar identity visibility, live-thread rejection, partial/aliased/bounded
copyout and personality separation. Run it with:

```sh
python3 scripts/test_native_credentials.py --output /absolute/new/output --sanitize
```

`tests/kernel/points/native_credentials_checks.hpp` reaches the same selector,
authorization and PCB mutation through `kernel_main` debug test points. This
boot diagnostic uses kernel-owned storage and explicit copy callbacks; it does
not demonstrate user-mode trap delivery. SDK-only OS guest qualification is a
separate execution gate.
