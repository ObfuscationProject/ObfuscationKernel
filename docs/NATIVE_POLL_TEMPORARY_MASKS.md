# Private ppoll and pselect temporary signal masks

The private signal fixture accepts nonnull ppoll/pselect masks. Ordinary builds
still reject nonnull masks with ENOSYS after the existing timeout, sigset-size,
and user-copy validation. All ten Linux profiles use their existing target word
size, byte order, and 8-byte or MIPS 16-byte kernel sigset representation.

The adapter copies and sanitizes the mask after copying the timeout, before
validating descriptors/fd sets. A syscall-local scope restores the original mask
on every completed return. Blocking transfers ownership to an internal record
bound to the thread entry-stack lease and the exact poll object/wait ticket. No
exported PCB, TCB, PollRequest, or SelectRequest layout changes; SDK ABI remains29.

The active delivery mask is temporary. A caught EINTR preserves it through signal
selection and handler setup, but the user signal frame saves the pre-wait mask.
The handler's active mask combines the temporary mask with its action mask and
signal unless SA_NODEFER applies. After frame commit the private record is gone;
sigreturn uses the saved user context, including legal edits to uc_sigmask.
Nested handlers and nested masked waits therefore restore their respective prior
masks rather than overwriting an outer frame's restoration authority.

Readiness, timeout, invalid arguments, failed input copy and failed poll revents
copy restore immediately. Remaining-time writeback remains best effort and does
not rebase a timeout. If ignored work leaves no handler, the original mask is
restored. Fatal actions, signal-frame failures, thread/group teardown and committed
exec clear owned records. Unrelated stale poll object/ticket cleanup cannot change
a newer record. Successful exec inherits the original mask if a deferred private
record still exists; failed exec leaves the previous state intact.

The atomicity argument is limited to the existing serialized boot-CPU runtime:
no user or IRQ signal producer can run between mask installation and registration.
A pending signal unblocked at admission is checked on the next recipient step.
Provider readiness and elapsed timeouts retain the normal-completion precedence.
O_NONBLOCK does not make poll itself nonblocking; poll still waits for readiness.

`test_native_poll_temporary_mask.py` runs actual runtime and scheduler code under
hosted synthetic traps, copy and clock backends. Its x86 execution checks real
signal-frame bytes and the sigreturn path; template adapter cases check all ten
ABI layouts without executing those CPUs. This is private hosted evidence, not
native hardware, SMP, process-directed signal or no-handler syscall restart support.
