# Owned pipe readv/writev continuations

This private successor of Kernel833 extends the existing pipe read/write objects
with value-owned iovec metadata. All ten kernel architecture profiles remain
required; hosted profile execution is distinct from native target execution.
The distribution continues to consume exported architecture/mode-specific SDKs.
Production caught signals remain disabled.

The dispatcher accepts at most 16 native `(base, length)` pairs and rejects a
summed length above INTPTR_MAX before checking payload mappings. It copies the
array once per syscall admission. Each object owns that complete copy, original
array pointer, numeric fd, retained open description, pipe identity, request and
exact wait ticket. A blocked retry never resolves the numeric fd or rereads the
array. Zero-count and zero-total calls return without an owned continuation after
the existing descriptor checks.

Reads check all nonempty copied spans, stage at most 4096 bytes, and scatter the
returned prefix in vector order. Writes own the full summed request and monotonically
advance a committed-byte cursor. Atomic writes retain eager payload validation;
large writes check/copy only the current unpublished suffix into bounded staging.
Blocking writes wait until the full request or a terminal condition. Nonblocking
writes return their available prefix. See `NATIVE_PIPE_BLOCKING_WRITES.md` for the
progress, fault, signal and diagnostic contract.

Typed origins distinguish read/readv and write/writev, validate fd/array/iovcnt
against captured arguments, derive request bytes from copied lengths, and bind the
full copied value. Rearm changes only the owned wait ticket. Write progress separately updates its
committed cursor and permanently disables caught-handler restart for that call. Invalid descriptors,
stale objects, substituted origins and old permits cannot authorize restart.
With zero progress, private EINTR returns -4 at the normal return continuation; SA_RESTART republishes
the original syscall instruction and arguments, then retires the old object.
Reexecution takes a fresh array snapshot and resolves the descriptor anew.
N64/PPC retain the existing typed architecture publication path.

Readiness, EOF/EPIPE, O_NONBLOCK, exec retirement, thread/process cancellation and
stale-wait cleanup use the existing directional pipe mechanisms. Requests at most PIPE_BUF retain their existing scalar/vector record formats.
Larger writes use the explicit LARGE namespaces documented separately. Vector records use exact READV/WRITEV namespaces:

- `OK_NATIVE_PIPE_READV_WAIT`, `OK_NATIVE_PIPE_READV_COMPLETE`,
  `OK_NATIVE_PIPE_READV_ORIGIN`, `OK_NATIVE_PIPE_READV_SIGNAL`.
- The corresponding `OK_NATIVE_PIPE_WRITEV_*` names identify writes.
- `array` is the original array address; `bytes` is the read staged sum or full write request; `iovcnt`
  is the admitted count. `vectors=base:length,...` includes every copied pair,
  including zero-length entries, in order with decimal integers.
- Each directional `live` count covers both scalar and vector objects in the
  shared pool. The separate namespace does not create a separate quota.

Author tests execute the actual dispatcher, object pools, scheduler, origin
tracker and private signal implementation with hosted hardware backends. They
cover metadata/FD replacement, scatter/gather, zero entries, capacity/overflow,
mapping faults before progress, nonblocking/EOF/EPIPE, 4096/4097 atomic/partial
boundaries, rearm, cancellation/exec, full origin-binding substitutions, caught
EINTR, sigreturn and a fresh array snapshot on restart. A boot debug test checks
metadata staging and identity only; it is not native continuation evidence.

The private Debug successor generates automatic SIGPIPE on terminal pipe EPIPE;
see `NATIVE_PIPE_SIGPIPE.md`. Production signals remain disabled.

No preadv/pwritev, SMP synchronization, physical hardware or M0-M12 completion is claimed by this slice.
