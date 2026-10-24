# Bounded process-group signal selection

The ABI73 / UAPI0.15 candidate retains current-group and negative-PGID
selection to the existing native and Linux `kill` boundaries. This does not
change their syscall numbers or add a group identifier layout. Each register
selector and signal uses its signed low 32 bits. Selection is serialized on
the boot CPU and uses canonical scheduler process groups.

For a selector of zero, the sender's current process group is selected.
A selector below minus one names the corresponding positive process group.
The implementation widens before negation and rejects INT_MIN with ESRCH.
Minus one remains unsupported and returns EOPNOTSUPP. Group calls admit only
signal zero, TERM (15) and KILL (9). Invalid signal numbers return EINVAL;
other valid group signals return EOPNOTSUPP. Positive-PID behavior remains
under its existing native or Linux signal contract.

Selection copies the sender's credentials, PID and PGID, then snapshots at
most 64 recipient PIDs. It retains no borrowed process or thread pointer
across delivery callbacks. Each recipient is resolved again and undergoes
the existing UID permission and execution checks. Signal zero checks
existence and permission without acquiring delivery state. Retained zombie
members retain their existing positive-target probe behavior; a process
group can survive its original leader's reap.

Recipients are independent: an allowed recipient may accept a signal even
when another member refuses. The call returns success if any recipient
accepts; otherwise it returns the last recipient error, or ESRCH for an
empty group. Accepted recipients are not rolled back. Native termination
is queued and serviced after publishing the sender's saved return frame,
before another user entry. A later TERM does not replace an accepted KILL.
This is bounded delivery at the existing serialized boundary, with no new
heap allocation or runtime virtual interface.

The exact `native_group_signal_profile` manifest field and profile header
are mandatory in matching architecture/mode SDKs. Producer and consumer
admission pin the bounded selectors, signal numbers, capacity and refusal
flags, together with session profile v3. All ten native/Linux profiles and
the four supplied musl profiles remain in scope.

This slice supplies no broadcast, HUP, stop/continue, terminal-generated
signals, same-session SIGCONT exception, PTY, interactive job control or SMP
behavior. Bounded [controlling-console authority](NATIVE_CONSOLE_CONTROL.md)
has its own capability; group-signal profile v2 acknowledges that association
without broadening the supported delivery signals.
Hosted selection/runtime checks, standalone Debug model points and actual
native guest execution are separate evidence classes. Development sources
or passing metadata checks alone do not qualify guest signal execution or
close a complete M0-M12 milestone gate.
