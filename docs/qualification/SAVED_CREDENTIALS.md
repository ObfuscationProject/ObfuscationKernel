# Saved user and group IDs

SDK/module ABI29 adds saved UID/GID to the kernel-owned `user::Credentials` PCB
record. The 28-byte record preserves the original real UID/GID, effective
UID/GID and kernel-space flag order, then appends saved UID/GID. This preserves
the meaning of legacy five-field positional initializers. Compile-time checks bind its offsets
on every target. ABI28 SDKs must not be mixed with this layout.

Account creation initializes saved IDs from the account IDs. An aggregate that
omits saved IDs initializes them from its effective IDs, preventing nonroot
callers from acquiring an accidental saved root identity. Trusted kernel APIs
for UserManager switching and credential installation remain separate from
untrusted Linux syscall requests. Fork copies the complete credential record;
successful ordinary exec refreshes saved IDs from effective IDs at image
commit. A rejected exec that returns to the old image preserves its credentials;
an existing fatal error after irreversible commit does not restore that image.

The native Linux adapter implements setuid/setgid, setreuid/setregid,
setresuid/setresgid and getresuid/getresgid on all ten architecture profiles.
Generated numbers are checked against pinned musl1.2.5 and Linux6.13.4 sources.
i386/ARM32 legacy16 calls and their uid32 variants remain distinct; the other
profiles use32-bit IDs even with64-bit registers. Legacy16 scalar getuid/getgid/geteuid/getegid
use the same65534 overflow conversion; exactly65535 is preserved. The shared
native boundary validates the caller before retrieving any scalar identity. Scalar identity completion
preserves high unsigned32 IDs even in the negative errno bit range on32-bit
profiles; ordinary syscall completion still rejects ambiguous success values.

Identity changes authorize all requested values against the immutable old
record and publish only a complete candidate. Unprivileged setuid/setgid may
select old real or saved IDs; setre* permits old real/effective for real and any
old real/effective/saved for effective; setres* permits any old tuple member for
each field. A supplied real ID, or an effective ID different from old real,
updates the saved ID on setre*. EUID zero supplies the selected privilege for
both UID and GID changes. EGID zero supplies none. This model does not implement
Linux capabilities, namespaces, securebits, LSM hooks or resource-charge limits.

Setuid/setgid reject unsigned32 -1. In re/res calls it means unchanged. Legacy16
arguments truncate to16 bits before65535 becomes unsigned32 -1. Getres copies
three snapshot values in real/effective/saved order using explicit target byte
order: four bytes per native ID, two per legacy ID. Legacy values above65535
map to the fixed overflow value65534. A failed later output does not undo an
earlier store, and aliased pointers observe the last successful store, matching
Linux's sequential put_user behavior. No copyout atomicity is claimed.

The present PCB shares credentials across its threads. Native identity setters
therefore return EOPNOTSUPP for any request with more than one live thread,
including no-op requests. Exited thread slots do not block the single-live-thread
case. Linux per-thread credential divergence and POSIX libc synchronization
remain separate gates; shared credential getters do not establish either one.

Supplementary groups, setfsuid/setfsgid, set-ID executable elevation, secure
exec/AT_SECURE and full P09 remain unimplemented. Filesystem checks continue to
use effective IDs. Cross-process signal authorization is unchanged; the current
private SI_TKILL sender UID still uses effective rather than Linux real UID and
requires the later signal-targeting correction. Saved IDs alone do not complete
credential/signal integration.

The hosted credentials suite exercises all96 setter/getter forms over ten
profiles through the real adapter and checked-copy boundary, with38 independent
policy cases, denial atomicity, width/endian canaries, partial/aliased outputs,
thread refusal, fork independence and exec reset. The adapter rejects mismatched or non-running canonical callers before user
memory decoding or mutation. Reachable debug process tests also cover reversible
and permanent drop and shell synchronization replacing stale saved root IDs. These tests distinguish hosted models
from separately collected native musl guest runs and physical acceptance.
