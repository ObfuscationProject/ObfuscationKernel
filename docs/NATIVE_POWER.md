# Native power reservation and queued commit

UAPI0.10.0 introduced `OK_NATIVE_POWER_VERSION=1` with native-only syscall1032
`OK_SYS_POWER_PREPARE(action)` and syscall1033 `OK_SYS_POWER_COMMIT(token)`.
The current SDK is ABI70/UAPI0.12.0 and retains that scalar wire contract.
All five unused register arguments must be zero. Every native invocation
authenticates the canonical running original PID1 leader: native user process,
original runtime incarnation, parent zero, one live thread, retained VM/entry
ownership and all six real/effective/saved UID/GID values zero. Root children,
sibling threads, token possession and a process name never grant authority.

Wire actions are HALT1, REBOOT2 and POWEROFF3, independent of the private C++
enum ordering. Unknown actions and malformed words return EINVAL. Defined but
unqualified actions return EOPNOTSUPP. HALT retains the x86_64 backend. The
optional reboot backend requires an explicit x86_64 PC/8042 build policy, and
the optional poweroff backend requires an explicit x86_64 QEMU PIIX4 PM policy.
The ABI remains available on all ten Kernel profiles.
The capability string `original-init-scalar-prepare-queued-commit-v1` describes
the boundary, not platform support for every action.

`native_pc8042_reboot` defaults to disabled and rejects non-x86_64 builds when
enabled. Its canonical definition is `OK_NATIVE_PC8042_REBOOT=1`; the SDK
manifest records `terminal_reboot_backend=x86_64-pc-8042-v1` exactly for that
selection and `unsupported` otherwise. Neither an x86_64 target nor a readable
controller status discovers or qualifies an arbitrary machine. Backend selection
does not change the scalar wire contract or class layout; the current SDK remains
ABI70/UAPI0.12.0.

The selected backend polls status port0x64 at most100000 times before PREPARE
can publish a token, rechecks during queued preflight and again at the final
terminal boundary, and sends command0xfe once only after input-buffer readiness.
The preflight does not write controller state. Final execution disables
interrupts first; an unavailable/busy controller or a returned reset command
enters permanent hold. Ordinary HALT and this fallback use the same allocated
`ok_arch_terminal_hold` symbol with bytes `fa f4 eb fc` (CLI, HLT, backward JMP).
QEMU reset observation, a subsequent boot, clean storage and a later HALT remain
separate evidence; source and hosted fake-port tests do not establish them.

`native_qemu_piix4_poweroff` also defaults to disabled and rejects non-x86_64
builds when enabled. Its canonical definition is
`OK_NATIVE_QEMU_PIIX4_POWEROFF=1`; the SDK manifest records
`terminal_poweroff_backend=x86_64-qemu-piix4-pm-v1` exactly for that selection
and `unsupported` otherwise. The qualified execution scope is one CPU on the
selected QEMU PIIX4 machine, not general x86_64 or physical-machine support.

This backend discovers the actual bus-zero PCI function. It validates the
selected Intel PIIX4 PM identity, class/revision, header/multifunction structure,
uniqueness and enabled bounded PMBASE, including exclusion of PCI configuration
port overlap. It does not guess a PM I/O address or parse general ACPI tables.
Non-consuming preflight reads configuration without writing PM control. At the
terminal boundary it revalidates the selected device, PMBASE and control value
with interrupts disabled, then requests S5 sleep type zero with SLP_EN while
preserving unrelated control bits. A failed check or returned poweroff request
enters permanent hold. The original coordinator and storage clean seal below
remain authoritative; the backend supplies only the final terminal action.

The bounded poweroff qualification includes twenty exact ABI70/UAPI0.12.0
Kernel SDK/SDK-only OS tuples and ten standalone Debug boots. Separate actual
x86_64 Debug and Release campaigns each perform two boots and two natural
shutdowns, reusing the same private disk for restart, password login and exact
persisted-file read. Independent readers bind the linked guest PM write, S5
device observation, guest-caused QMP SHUTDOWN, natural QEMU exit zero and final
read-only filesystem/account checks. Hosted models remain separate evidence.
The original canonical validator retains a final-guard failure for two vanished
xmake probe temporaries. Separate read-only re-admission checks all 37 successful
commands and unchanged products without rebuilding or rerunning guests.
Natural emulator teardown can flush block state; this is bounded orderly
shutdown and restart evidence. Production ext4 remains `<1,9>` with signed32
metadata time and no paired I/O.
These results do not qualify torn writes, volatile-cache loss, general power
failure, general ACPI, other platform backends, physical machines or complete
M0-M12 gates. Physical M11 and manual desktop M12 acceptance remain deferred.

PREPARE performs non-consuming runtime/producer/backend/storage preflight before
OS service retirement. It returns a positive boot-local selector no larger than
`OK_POWER_TOKEN_MAX=0x3fffffffU`. The current coordinator has one reservation per
boot and issues token1. Repeated same-action PREPARE is idempotent after caller
authentication; another action returns EBUSY. Failed preflight publishes no token.
There is no reset, cancellation or second reservation lifetime.

The selector is not secret and carries no pointer. The complete private
`ShutdownTicket`, owner and generation stay in Kernel memory. COMMIT authenticates
again, rejects an unissued/mismatched token with EINVAL, checks actual quiescence,
then queues the retained ticket. An unreaped child or incomplete owner drain
returns EBUSY without cleanup. Authorization failures return EPERM and invariant
failures propagate their stable errno. Arguments are full native words: 64-bit
high bits are rejected before narrowing. Scalars fit signed32 positive returns
and do not need endian-dependent records or split64 argument conventions.

COMMIT also requires no active console lease/buffer, pathname UNIX endpoint,
prepared accept/receive/descriptor operation or retained VFS socket-path token.
These checks use original ownership counters and have no pathname or PID
exemptions. PREPARE still permits live service resources. PID1 closes its control
listener, accepted clients and readiness channels before COMMIT. Final endpoint
close consumes backlog and binding references; a linked socket filename itself
does not require an unlink step. Ordinary stdio/cwd owners remain for the trusted
outer resource drain.

Zero from COMMIT only means queued. The actual native step publishes its return
frame and exits all step locals. Its pending guard prevents another ordinary
user entry. Only the actual desktop outer boundary can advance the coordinator,
close admission, stop producers, retire native owners, drain references, seal
the original storage owner and consume the terminal action. No cleanup or
privileged terminal instruction runs on the committing syscall stack. Failure
after queuing enters permanent diagnostic hold; no cleanup stage is retried.

The SDK exporter and OS consumer require the current UAPI0.12.0 header, ABI70, canonical
`ok/user/native_power_profile.hpp` and hashed artifact manifest capability. This
does not authorize mixing headers from a source checkout with an older installed
library. Compile controls cover all ten native C/C++ ABIs and emitted target-endian
wire constants. Hosted actual-dispatch controls use mock trap/VM/storage seams;
they do not establish guest halt, physical storage durability or human acceptance.
The reachable Debug `native-power/wire-no-init-refusal-model` point validates
wire constants and actual adapter refusal before an original native init exists;
it neither invents an init nor reserves or queues shutdown in the standalone test
kernel.
