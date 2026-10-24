# Native process sessions and process groups

The ABI73 / UAPI0.15 candidate exposes `setsid`, `setpgid`, `getpgid`,
`getsid` and `getpgrp`. These scalar calls operate on canonical scheduler
process ownership. Their native numbers are 112, 109, 121, 124 and 111,
respectively. Linux compatibility uses the generated Linux 6.13.4 / musl
1.2.5 number tables for all ten retained architecture profiles. Linux PID
arguments use the signed low 32 bits of each register.

A newly created independent process owns its SID and PGID. PID1 therefore
starts in session/group 1. A forked child inherits the actual parent's SID
and PGID. Fork starts a new child-exec epoch: only a successful image
replacement marks that child as executed. Exec preserves SID and PGID;
failed exec does not change the marker. Reparenting changes neither ID.

`setsid` creates a session and group named by the caller's PID, unless that
PID already names a retained process group or the caller is a session
leader. `setpgid` may target the caller or its child. Changing an executed
child returns EACCES. An unrelated process returns ESRCH. A session leader,
a child in another session, and joining a nonexistent or foreign group
return EPERM. A negative group ID returns EINVAL. Retained zombie members
reserve group identities until reaped. A group can outlive its original
leader. Getters accept live nonleader TIDs as well as retained leader PIDs;
retired nonleaders and reaped leaders return ESRCH.

The mechanism currently runs at the existing serialized boot-CPU syscall
boundary. It adds no process table allocation, runtime virtual dispatch,
blocking operation or new architecture-specific scheduler path. The module
ABI originally changed with the exported PCB layout and exec-lifecycle state.
The current cohort requires session profile v3 and a separate bounded
[process-group signal contract](NATIVE_PROCESS_GROUP_SIGNALS.md). Session
profile v2 removes the former broad `group_signals` boolean; the separate
profile enumerates the supported selectors and signals. Session profile v3
adds the bounded [controlling-console capability](NATIVE_CONSOLE_CONTROL.md).
The exporter and OS consumer require the exact session capability profile
and matching architecture/mode artifacts. The public musl supply matrix
remains x86_64, aarch64, rv64 and loongarch64; all ten native profiles remain.

Distribution login policy belongs to ObfuscationOS. Authenticated login
closes its account registries, creates a session, verifies SID/PGID equal its
PID, explicitly acquires and checks its console, enters the selected home and
applies credentials, then rechecks the session and console before directly
executing the shell. Existing console generation
leases retain their independent input-authority contract.

Session creation alone does not acquire a controlling console. The separate
explicit acquisition and foreground contract creates no PTY, broadcast signals,
stop/continue jobs or same-session SIGCONT exception. Session membership
alone does not establish those properties or prove process isolation.

Validation separates hosted scheduler lifecycle checks, hosted actual
native/Linux syscall dispatch, Debug boot model checks and actual native
QEMU process/exec tests. Qualification receipts under
`build/qualification/native-sessions-candidate-v1` record which layers have
executed. No physical-machine, SMP or complete M0-M12 acceptance follows
from this bounded feature.
