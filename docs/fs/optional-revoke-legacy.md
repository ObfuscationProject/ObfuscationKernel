# Bounded legacy writes with an optional revoke feature

This private ABI61 source successor keeps Linux UAPI 0.7.0, ten Kernel
architectures, twenty Debug/Release SDK rows and four supplied musl platforms.
It extends the existing canonical legacy completion families to exact JBD2
feature words checksum-v3 and revoke|checksum-v3. Unknown features, checksum-v2,
64-bit tags and async commit remain outside this completion contract.

`ReplayPlan::revoke_records` counts validated active revoke record blocks,
including empty records and uncommitted records. `revoked_actions` counts only
suppressed committed actions. Stale boundary records are validated but are not
active. General JBD2 parsing/replay retains revoke semantics. The existing
bounded ext4 canonical admission guards additionally require zero active revoke
records; no revoke record can be hidden by a zero suppression count.

The completion change preserves ownership, home/payload checksums, action
counts/order, dirty/clean state, journal sequence, capacity, no-I/O preparation,
ordered-data FLUSH, checkpoint, Session identity and quarantine requirements.
The public CommonStorage path reuses its existing typed zero/refill and legacy
ordered writer for later nonempty writes; it introduces no second writer.

The common-opening SDK profile is
`serialized-common-seven-family-zero-refill-optional-revoke-legacy-v2`.
Module/core, common storage, shared-file, TCP and UDP ABI markers all reserve 61.
Their existing non-storage capability profiles remain unchanged. SDK export and
the OS artifact validator require the same module ABI and common-opening string.
The old ABI60 profile and headers remain separate frozen artifacts.

Useful existing models have new cases for non-suppressing, empty, uncommitted
and stale-boundary revoke records, canonical completion refusal, both feature
words in legacy writer/recovery families, and repeated staged word/space/newline
saves through public CommonStorage. Existing lifetime, malformed-media,
partial-I/O, ordering, quarantine and fault/recovery controls remain present.
These cases are authored only until root executes the separately pinned recipes.

This source does not establish native Notes persistence, general/default ext4,
large-file support, isolated applications or desktop, synchronized shutdown,
hardware cache power-loss safety, IRQ/SMP, physical hardware or full M0-M12.
Model compilation/execution, matching standalone Kernel/SDK-only OS builds and
native cold-media/Linux verification are separate qualification steps.
