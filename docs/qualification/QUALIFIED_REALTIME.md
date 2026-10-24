# Qualified epoch mechanisms and platform diagnostics

The shared SDK/module17 and native UAPI0.5.0 integration adds checked public
CLOCK_REALTIME reads. Its separate all-ten native/musl and fixed-date R3
qualification passed. See `PUBLIC_REALTIME.md` for the source capability and
remaining limits; privileged platform diagnostics alone are not user-mode evidence.
The following foundation history describes its earlier SDK15 scope; historical
evidence does not certify the new public candidate.

The original slice exported internal UNIX epoch mechanisms and retained-QEMU platform
diagnostics in SDK/module ABI15. Native UAPI0.4.0 was unchanged. It did not enable
public CLOCK_REALTIME, musl time/gettimeofday, maintained filesystem timestamps,
or Linux stat. Native runtime clock adapters were monotonic-only. The internal
legacy time/gettimeofday and ID-0 model alias had boot-relative semantics;
the public candidate deliberately migrates these paths to the checked provider.

For the existing monotonic adapters, nonnull `clock_getres` output requires a
successful monotonic sample before encoding resolution. Both Linux and native
paths return EIO without user-buffer writes when the counter is uninitialized or
invalidated. NULL `clock_getres` remains a clock-identity-only check in both paths:
clock1 succeeds even while unavailable, unsupported identities fail EINVAL, and
no counter read or user copy occurs. The foundation alone did not enable
realtime clock0; the public candidate supplies its separate checked provider.

`ok/time/epoch.hpp` checks Gregorian calendars in years1..9999 using UTC and POSIX
seconds, with no hosted library, floating-point conversion or guessed century.
Second60 is rejected. The internal value uses signed64 seconds and normalized
nanoseconds. Addition splits elapsed nanoseconds before adding seconds and checks
the carry, preserving negative epochs and avoiding full-epoch nanosecond overflow.

`EpochAnchor` is noncopyable and has uninitialized, unavailable, ready and
invalidated states. Its first initialization attempt is terminal: success is
idempotent without rebasing, failure remains unavailable. A source read is
bracketed by samples from the same nonzero monotonic generation and must complete
within100ms. It anchors the sampled source at the trailing monotonic sample and
records the acquisition span and source resolution. Accuracy remains explicitly
unknown; nanosecond interpolation does not imply nanosecond UTC accuracy.
Backwards samples, changed generations or arithmetic exhaustion invalidate the
anchor. A caller cannot revive it with reinitialization.

`ok/arch/clock.hpp` now exposes a boot-generation sample and explicit terminal
`invalidate_clock()`. Initialization failure is preserved. Counter sampling
overflow invalidates the clock; initialization never restarts it. Known platform
reset, suspend, restore or frequency changes must call invalidation before that
transition. This interface does not discover those events automatically, and no
new suspend/migration implementation is supplied. `clock_status()` and realtime
information/initialization queries cannot report a ready anchor after known
clock invalidation.

The existing narrow-counter limitation remains: MIPS CP0 Count must be sampled
at least once per32-bit wrap. Multiple unseen wraps cannot be detected by a
generation token or converter. There is no arbitrary stopped-VM, physical
hardware or SMP claim. All calls remain serialized on the boot CPU; access is
qualified for the retained platform's mapped device addresses, including the
early boot diagnostic context. There is no virtual service on this selected path.

## Selected source readers

| Profiles | Source and behavior |
| --- | --- |
| arm32, aarch64 | Plain ARM PL031 at0x09010000. Check PrimeCell identity, ignoring the documented PID2 revision nibble; check the enabled counter before/after a32-bit DR read. Report unsigned epoch seconds at one-second source resolution. |
| rv32, rv64 | Goldfish RTC at0x00101000. Read LOW then its latched HIGH using32-bit little-endian MMIO and device fences; split the unsigned64 nanosecond epoch. |
| loongarch64 | LS7A at0x100d0100. Require TOY and oscillator enable bits, take bounded repeated complete calendar/year snapshots, validate Gregorian fields. The retained QEMU initial controls were observed disabled, and this code returns unavailable without enabling them. Native receipts identify the actual control-read result. |
| i386, x86_64, mips, mips64 | Explicitly unavailable: MC146818 full century provenance has not been qualified. This reader does not access CMOS. Existing Malta monotonic calibration still reads RTC UIP under its prior contract. |
| ppc | Explicitly unavailable: DS1338 full era and MPC I2C transport remain unqualified. No I2C transaction is attempted. |

Readers never write RTC calendar, control, trim or alarm registers. They have no
compiler-date, fixed-year, uptime or invalid-value fallback. The unsigned PL031
and Goldfish domains must not have wrapped; the first sample cannot infer a lost
era. These are selected board contracts, not runtime device discovery.

The launch must establish a UTC RTC base. Fixed UTC fixtures in tests are oracles,
not production provenance. Qualification uses `clock=vm` and continuous guest
execution. A one-time anchor does not follow subsequent host wall-clock changes
or suspended wall time. Clock resolution and unknown accuracy remain distinct.
PL031 revision matching follows Linux6.13.4 `drivers/rtc/rtc-pl031.c` amba ID
0x00041031/mask0x000fffff; Goldfish LOW/HIGH latching follows
`include/clocksource/timer-goldfish.h`; LS7A fields and controls follow
`drivers/rtc/rtc-loongson.c`. Exact local references and installed QEMU model
evidence are pinned in the isolated candidate handoff. No QEMU source has been
vendored or claimed to have been inspected.

## Validation and remaining gates

`scripts/test_realtime.py` compiles hosted tests at O0/O2. It supplies39,997 date
vectors from Python's independent datetime arithmetic, plus wide-integer elapsed
oracles, invalid calendars, register sequences, stopped/unsupported RTC inputs,
torn snapshots, acquisition timeout, generation loss, rollback and overflow.
`--sanitize` enables ASan/UBSan. In ptrace-restricted environments LSan requires
`ASAN_OPTIONS=detect_leaks=0`; report this explicitly, without claiming leak
validation. The original counter tests are retained separately.

A small epoch model point is reachable through the standalone debug-kernel
test suite. It is still model coverage even when booted in QEMU.

The separate OS `tools/test_realtime.py` snapshots one exact architecture/mode
SDK17, validates every member, links SDK libraries and boot object, and executes
the platform diagnostic. It never builds kernel source. Positive source cases
exercise1970 epoch zero,2024 leap day and2040 time64 dates, actual RTC and anchor
progress, idempotence and terminal known invalidation. All profiles also exercise
initialization before monotonic time and prove that the failed anchor never
retries. Unavailable profiles are reported as refusals, not positive RTC support.
The receipts pin QEMU, SDK, probe source, binary and output hashes. This is native
privileged platform execution, not userspace clock syscalls.

The public candidate migrates the internal ID-0 alias and legacy compatibility
calls, retaining target-endian/time32 overflow and nonmutating copyout behavior.
Fresh native and musl public execution remains a separate gate from these
privileged diagnostic receipts. musl1.2.5 `time`/`gettimeofday`
ignore the error from CLOCK_REALTIME and therefore cannot be advertised on an
unavailable provider simply because the kernel returns an error. Writable
timestamp mutation/recovery policy and complete Linux stat metadata remain
separate gates. Preserve all ten Kernel and OS userland profiles throughout
those changes.
