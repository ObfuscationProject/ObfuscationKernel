# Explicit administrative realtime setting

The shared SDK/module19, native UAPI0.6.0 boundary adds clock_settime. The frozen
759-file all-ten Debug/Release producer/consumer matrix and a fresh shared x86
rebuild passed independently audited guest and artifact gates. The prior read-only
clock history is recorded in PUBLIC_REALTIME.md and QUALIFIED_REALTIME.md.

Native syscall227 takes a clock identity and a pointer to the existing16-byte
record containing two signed64 fields. Only CLOCK_REALTIME (0) can be set.
Linux uses the pinned Linux6.13.4/musl1.2.5 target declarations: ten architecture
profiles and14 declared syscall forms. Operations append to the generated enum;
old native numbers and Linux operation values remain unchanged. Native copies
preserve all64 nanosecond bits; Linux32 kernel-time64 records ignore the high32
nanosecond padding exactly as the selected Linux conversion does. Target byte
order and target errno conversion apply at the Linux boundary.

The checked user boundary admits the clock identity, copies the entire record,
validates time, checks process authority, samples monotonic time once, checks the
monotonic floor, then commits. Supported seconds are0 <= seconds <8277292036;
nanoseconds are0 <= nanos <1000000000. This is the Linux TIME_SETTOD_SEC_MAX
range, not the internal Gregorian conversion range. The whole requested pair
must be at least the sampled monotonic elapsed time; epoch0 generally cannot be
set after boot. RTC-derived epoch0 reads remain valid. Invalid identity/range
returns EINVAL, invalid input copies EFAULT, insufficient authority EPERM, and
unavailable or discontinuous monotonic time EIO. No user supplies credentials
or a monotonic anchor. Authority is effective UID0 or a trusted kernel-owned
credential; this subset is not Linux CAP_SYS_TIME/user-namespace support.

RealtimeClock retains the one-shot physical boot EpochAnchor separately from
the selected software basis. A successful explicit set publishes the epoch,
monotonic anchor/generation, incremented wall generation, conversion resolution
and administrator_software origin. UTC accuracy remains unknown. Reads add
monotonic elapsed time to that basis; forward and backward wall steps never
reinitialize monotonic time or adjust its pending relative/absolute deadlines.
An unavailable physical RTC can therefore coexist with an explicitly available
software-set wall clock. realtime_info reports the retained physical diagnostic;
realtime_clock_info and realtime_sample report the active source. Repeating
boot initialization preserves the original boot outcome without changing the
active selection. No physical RTC calendar/control register is written.

Monotonic rollback/generation failure or epoch arithmetic exhaustion invalidates
the active clock. There is no fallback to a stale physical anchor after software
selection fails and no public reset/unset operation. Validation and authority
failures leave the selected basis unchanged. Wall generation exhaustion is
checked before publication; current model evidence covers its helper arithmetic,
not a complete coordinator advanced through UINT64_MAX successful settings.

The execution contract is one serialized boot-CPU main loop. Hardware user entry
returns a trap before the loop dispatches a syscall; current IRQ handlers neither
call realtime services nor schedule a nested dispatcher. This is not a claim
that all kernel execution has IRQs masked. The clock has no lock: IRQ/NMI callers
and secondary CPUs must not use it. Future IRQ integration must protect the
whole monotonic-sample-through-read/commit interval and underlying monotonic
provider state, preserving prior IRQ state. Guarding only the final assignment
would leave stale-sample races. SMP requires a separate synchronization design.

The coordinator and adapter models, target compilations, privileged platform
provenance probes, actual native/musl user execution and disposable Linux reference
boots are separate evidence layers. A privileged fixture may assign initial
UID1000 before first user entry and observe blocked threads between user steps;
it is not a public credential syscall. Poll/select deadlines remain private to
the runtime: the fixture observes blocked pending syscalls and correlates actual
user monotonic elapsed checks; it does not read their private timer objects.

Absolute realtime timers, signals/restart/cancellation, NTP/adjtime, suspend,
migration, filesystem timestamp maintenance, Linux stat, complete libc math,
physical hardware and SMP qualification remain open. The empty PT_TLS admission
fix preserves duplicate/range/alignment/nonempty-template rejection; it addresses
a real exec prerequisite without supplying general dynamic linking or TLS threads.
