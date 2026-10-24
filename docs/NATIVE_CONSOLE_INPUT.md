# Native console input lease

The SDK capability is
`native_console_input_profile=pid1-issued-epoch-fork-exec-leader-revoke-noecho-v1`.
Its fixed declarations are in `ok/sched/console_input_profile.hpp` and
`ok/uapi/syscall.h`. This additive interface was introduced in UAPI 0.8.0.
The current candidate UAPI is 0.15.0 and module ABI is 73. The scheduler and private PCB
layout are built together inside the matching Kernel artifact.

All ten Kernel profiles use the same native numbers. Unused arguments must be
zero. These controls are unavailable through the Linux personality.

| Call | Number | Argument | Success |
| --- | --- | --- | --- |
| `console_claim` | 1028 | Fresh direct child PID | Positive issued epoch |
| `console_revoke` | 1029 | Expected epoch | Zero |
| `console_flush` | 1030 | Expected epoch | Zero |

Input starts closed. Only the original boot PID 1 can issue or revoke a lease;
fork and adoption never inherit that administrative authority. A claim requires
a direct user child with exactly one live thread, no children and no previously
issued membership. PID 1 must gate the child before publishing the lease. A
nonzero effective UID is neither necessary nor sufficient for input ownership,
and root services have no bypass.

The scheduler writes a private, read-only-to-callers PCB epoch. Fork inherits
only a currently active epoch; successful exec keeps the membership. Every
native console `read`, `readv` and readiness operation checks this membership,
including Linux calls routed through the common native input path and duplicate
console descriptors. A blocked read retains its original epoch across a wake;
a stale continuation returns `EACCES` before consuming a later owner's input.
Poll/select retain their original epoch and report `POLLERR|POLLHUP` after
revocation without revealing a later owner's queue occupancy.

The active leader or PID 1 can flush. A descendant cannot flush the leader's
input. Revoke, leader exit, the leader's final thread exit and PID 1 exit close
the entire epoch, erase buffered input and cancel its blocked readers. Adoption
and PCB table compaction preserve old membership values without renewing them.
An epoch cannot wrap: issuance stops at `0x3fffffff`, which fits every native
word size. A new login needs a new child after revoke or failed issuance.

The 256-byte queue drops input while closed or full. It never echoes bytes.
Consumed cells and the entire queue at a close/flush are explicitly zeroed.
Claim, revoke and flush also drain the platform serial input callback, polling
at most 1,024 times. A source that cannot become empty fails closed with
`EIO`; a missing freestanding callback fails closed with `EOPNOTSUPP`. This is
the serialized boot-CPU serial input mechanism. GUI keyboard dispatch, terminal
line discipline and interrupt/SMP synchronization remain separate work. Explicit
[controlling-console and foreground-group authority](NATIVE_CONSOLE_CONTROL.md)
is a separate layer under this unchanged lease mechanism.

The hosted actual-dispatch fixture uses mock trap/copy/mapping backends:

```sh
python3 scripts/test_console_input.py --output /absolute/fresh/output
python3 scripts/test_console_input.py --output /absolute/fresh/sanitized-output --sanitize
python3 tests/host/console_input_sdk_contract_tests.py
```

The debug `console-input / lease-authority-model` test point is reached through
`kernel_main`. It checks the lease algorithm with kernel-owned storage. Neither
that model nor the hosted fixture proves guest trap delivery or implements
account authentication, password storage or a login frontend. The OS must consume
the matching exported architecture/mode SDK and qualify its own user-mode flow.
