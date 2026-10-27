# Native kernel entry stack ownership (SDK26)

Scheduled native execution uses `arch::execution::enter_owned` with a
pool-minted active lease. The two-argument `enter` remains available only to
standalone architecture diagnostics. Both interfaces are statically selected.
The scheduled path has no fallback to a shared entry stack.

`KernelEntryStackPool` owns 128 stable records and an explicitly supplied,
4096-aligned arena in kernel BSS. Each slot contains 65,536 usable bytes and
4,096 prefix/suffix canary bytes: 73,728 bytes per slot, 9 MiB total. The final
linked image includes the arena in `_kernel_start`/`_kernel_end`. Firmware frame
qualification excludes that image where supported. Linker-backed profiles keep
the arena separate from their native user-frame array. These are bounded
bootstrap charges, not runtime-sized quotas or general kernel allocation.

A TCB holds a nonowning pool/slot/generation handle and a checked cached top.
Copies during scheduler-vector compaction never release backing. Pool records
carry PID/TID, nonwrapping generation and activation epoch. A copied view from
an earlier activation never revives. Canary corruption quarantines the slot,
retains its charge, and closes allocation/entry. Other healthy inactive slots
can still retire. Canary pages are mapped supervisor memory, not faulting guards.

Creation reserves a lease before runnable publication. Linux clone uses a
constrained preparation callback on an unpublished TCB before parent-TID stores.
Native fork leaves its child created until its address space is built, entry-stack
preparation and return-frame setup succeed. Init likewise prepares before
`set_runnable`. Resource failure leaves existing owners intact.

Each boot-CPU entry validates its lease and canaries, activates one view and
passes its top to the architecture backend. The trap records an actual privileged
SP or assembly capture-frame base. Syscalls and timers require a nonzero capture
inside that lease; pre-entry frame rejection may have no capture. Backends
restore the service stack and detach TSS/global/scratch references before the
runtime deactivates the view. The runtime dispatches syscalls only afterward.
Entry switching is locally interrupt-masked; this is not concurrent SMP safety.

Thread/group exit retires inactive leases before scheduler destruction. The
scheduler refuses exit, image replacement, reaping and retired-slot reuse while
a handle or stack top remains attached. Exec preflights all leases, retains
handles in local escrow and reserves a leader lease when the old leader has
retired. A scheduler failure restores all old references. A successful commit
retains the leader lease and retires peers. The result distinguishes precommit
failure from postcommit corruption. Postcommit corruption stops native execution,
retains both VM generations and every unreleased peer handle, and cannot return
a normal exec error into the old image. Unexpected preparation corruption also
retains its handle in stable terminal storage before discarding an unpublished
TCB.

`OK_ENTRY_STACKS version=1` reports separate arena accounting and cumulative
validated syscall/timer/fault captures. It is evidence only when produced by a
pinned actual guest; hosted stubs use explicitly synthetic captures. Hosted pool,
scheduler, clone and exec tests do not prove architecture switching. The OS
artifact-only thread qualifier checks native clone/TLS/exit and independently
checks lease accounting for each command. Full standalone and OS-linked images,
all ten architectures and both modes must be qualified for each source generation.

This implements owned entry/capture stacks. Waiting syscalls still resume via
saved user frames on the service loop; parked C++ kernel continuations, nested
fault recovery, real faulting guards, complete signals/pthreads, dynamic stack
quotas and SMP remain separate work. It does not close M1, M3 or M0–M12.
