# Serialized native thread foundation (SDK26)

The scheduler uses one non-reused positive signed-32-bit task-ID domain. A
process leader's TID equals its PID/TGID; peer threads consume the same sequence.
Retired nonleader slots can be reused only with fresh identities, no active wait
registration and no owned kernel stack. Retirement does not release group VM,
file descriptions, cwd or credentials. All-retired groups reject thread creation,
fork, runnable activation and exec. No-argument current-task services refer to
CPU0; per-CPU model selection has explicit CPU accessors.

Linux raw clone admits the VM|FS|FILES|SIGHAND|THREAD sharing profile, optionally
SYSVSEM, SETTLS, PARENT_SETTID, CHILD_SETTID, CHILD_CLEARTID and legacy DETACHED.
Other sharing/namespace/signal arrangements remain unsupported. Invalid
THREAD/SIGHAND/VM dependencies return EINVAL. Legacy flags use their low32 bits.
x86_64 and LoongArch use child_tid before TLS; the other retained profiles use
TLS before child_tid. O32 copies only the consumed fifth stack word, with native
Linux stack-sign validation and fault-to-zero behavior. The i386 TLS descriptor
subset is the existing flat entry7, limit0xfffff, flags0x51; clone never allocates
or writes back this descriptor. x86_64 admits the four-level TLS address domain.
Other register TLS values are not dereferenced or required to be mapped.

The child inherits the complete caller frame, with ABI-correct zero result,
stack register mirrors and optional TLS replacement. Parent-TID stores are four
bytes in target endian and ignore copy failure after publication. CHILD_SETTID
is deferred to the child's first user entry, and its copy failure is ignored.
A zero raw child stack inherits the caller's stack; nonzero stacks are copied
without artificial alignment requirements. The execution backend still checks
safe native entry and contains user faults.

Linux raw exit retires only the caller; exit_group and the native personality's
legacy exit retain group teardown. The last live thread publishes the group
exit status and releases shared resources. Clear-child-TID operates only while
another address-space user remains: the store is attempted, its failure ignored,
and an independently validated shared-operation wake of one waiter follows.

Futex keys now include a domain tag. PRIVATE operations use PID+virtual word;
ordinary WAIT/WAKE on this runtime's eager private anonymous backing use a
different tag and require writable user backing even for WAIT. These domains
never alias. Shared anonymous mappings now exist, but shared-frame futex
WAIT/WAKE returns `ENOSYS` until a retained frame-generation key exists.
Coherent file mappings and cross-PID CLONE_VM also require an explicit backing
identity service. Robust, PI, requeue, bitset,
signal interruption and concurrent IRQ/SMP queues remain separate work.

Native execution remains serialized on the boot CPU with [owned per-thread
entry stacks](NATIVE_ENTRY_STACKS.md). This is not parked C++ continuations,
concurrent kernel entry, complete signals/pthread semantics, dynamic process/thread quotas,
real SMP, physical qualification or completion of P05/M3. The ten-architecture
matrix remains mandatory. Hosted mechanisms, compiled probes and actual guest
execution have distinct evidence receipts.
