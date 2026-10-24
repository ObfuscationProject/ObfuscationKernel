# Owned restart publication prerequisite

The private `src/user/native_syscall_restart_publication.hpp` coordinates the
owned syscall-origin table with the n64 and PPC persistent signal readiness
records. It accepts an opaque origin permit, validates the actual owner, complete
capture, exact lease and pending pipe binding, and stages a typed transaction.
Commit revalidates and consumes that permit before publishing the restart frame.
An old token cannot revoke a later capture or a completed restart. Canceling the
current staged transaction fails closed.

The two image-generation counters have distinct issuers. Each authority checks
its own current generation; they are joined through the same pool/slot/lease,
PID/TID, address space and captured frame, not numerical equality of counters.
A reactivated entry lease invalidates origin resolution even if its handle and
frame bytes are unchanged. Space-update preflight checks that retained epoch too.

n64 publication restores the captured input registers and original syscall PC,
then retires ordinary completion and child-derivation authority. PPC additionally
keeps CR0.SO set and publishes trusted diagnostic result zero while preserving
original r3/trap provenance. This is not `set_result(0)` followed by a PC edit.
Signal context codecs explicitly admit the prepared-restart context. Sigreturn
continues to restore user edits without implicit replay.

This prerequisite does not enable caught pipe interruption or SA_RESTART in the
runtime. The future caller must serialize selection, publication, old wait/pin
retirement and handler construction with no scheduling/user boundary. MIPS BD
remains unsupported. Production signal dispatch remains disabled. No SMP or
physical-machine claim follows from the hosted checks.

The hosted runner is `scripts/test_native_syscall_restart_publication.py`.
Supply `--output`, `--ppc-elf` and `--ppc-elf-sha256`; the ELF must be the pinned
Linux PPC32 scalar-soft-float image used for real admission metadata. Mock backend
observations, real pool/scheduler records and hosted user-copy stubs are explicitly
separate from native user execution. The origin runtime regression additionally
covers retained-epoch rejection on all ten selected architecture profiles.
