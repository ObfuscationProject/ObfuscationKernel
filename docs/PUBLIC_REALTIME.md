# Checked public realtime clocks

The shared integration requires matching SDK/module17 and native UAPI0.5.0.
Native legacy time/gettimeofday uses checked UNIX epoch time. The frozen R3
all-ten Debug/Release producer/consumer matrix, native/musl parent/fork/exec,
additional epoch0/2040 boots on all four positive profiles and a forced shared
x86 rebuild passed. Source capability and broader limits below remain explicit.

The native runtime initializes one RTC anchor after execution mappings and the
monotonic clock, before the initial image runs. Unavailable RTC does not prevent
boot. Fork, exec, process switching and runtime retry never rebase or retry the
first acquisition. Capability diagnostics retain unknown UTC accuracy, source
resolution and acquisition interval. No RTC register is written.

Native and Linux clock_gettime/getres support realtime0 and monotonic1 through
one checked hardware provider. Unsupported IDs return EINVAL. Valid NULL getres
is identity-only and succeeds without sampling. Nonnull output requires a live
provider; uninitialized, unavailable or invalidated clocks return EIO without
touching output. Resolution reports the interpolating counter's output step,
not RTC resolution or UTC accuracy. Absolute realtime sleeps remain unsupported.

Linux output uses each target's declared legacy or kernel-time64 layout and
byte order. Time32 overflow returns EOVERFLOW before user copying. Native clock
and timeval records retain signed64 seconds. No epoch-nanosecond multiplication
is used. Complete records are staged before checked user copying; invalid
output ranges return EFAULT without partial publication.

Native time returns a single signed target word and optionally copies signed64
seconds. It returns EOVERFLOW before any output for a value outside that word's
signed range or in the native errno collision range[-4095,-1]. Other negative
seconds remain representable; signed clock and timeval records are independent
of this legacy return limitation. Native gettimeofday truncates nanoseconds to
microseconds, ignores its historical timezone argument and accepts NULL timeval
without sampling. The trusted syscall table shares the epoch provider but keeps
its explicitly simulated monotonic and sleep model separate.

Only retained QEMU ARM32/AArch64/RV32/RV64 profiles have positive foundation RTC
qualification, within the nonwrapped source range. The other six retain honest
unavailability. Source, privilege, endianness and MMIO lifecycle constraints from
`QUALIFIED_REALTIME.md` still apply. SMP, suspend, migration, clock setting,
filesystem timestamps, Linux stat and physical hardware are outside this slice.

Pinned musl1.2.5 clock_gettime handles syscall errors; time/gettimeofday wrappers
ignore failure. Positive wrapper tests require a successful realtime preflight.
Unavailable platforms may claim the clock_gettime error path only, not safe
upstream wrapper failure behavior. No musl source patch or raw Linux legacy time
syscall is added. Provider failures use EIO, never ENOSYS fallback.

On legacy-time32 profiles musl1.2.5 clock_getres always supplies a temporary
record to the old syscall, even for a NULL public pointer. Its NULL wrapper call
therefore reports EIO on unavailable sources. The raw Linux NULL syscall remains
an identity-only success. The user probe checks both behaviors explicitly;
time64-only and64-bit wrappers preserve the NULL argument.
