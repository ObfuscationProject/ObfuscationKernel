# Native platform clocks

`ok/arch/clock.hpp` exports a boot-CPU monotonic clock. `initialize_clock()`
establishes the epoch once and preserves initialization failures. Subsequent
initialization succeeds without moving the epoch. `monotonic_nanoseconds()`
samples an actual free-running counter, including elapsed idle and kernel work;
it does not count timer-trap returns. `clock_resolution_ns()` reports counter
granularity rounded upward to whole nanoseconds, or zero before initialization.
Resolution is not a calibration-accuracy claim.

The implementation selects a concrete `PlatformClock` at compile time. There is
no virtual service or synthetic counter fallback. It supports the ten retained
QEMU platform profiles with the following source contracts:

| Profiles | Counter | Frequency source and boundary |
| --- | --- | --- |
| x86_64, i386 | Serialized TSC | Measured over approximately 200 ms against the PC PIT's 1,193,182 Hz input: channel 2 free-runs in mode 0, each end is the tightest of eight latched count reads bracketed by TSC reads, and wraps are summed from reads taken more often than one 55 ms wrap (an attempt with a longer gap is repeated). Channel 2 is reserved during boot calibration; channel 0 preemption remains independent. Requires the retained PC CPU profile's TSC and a stable QEMU TSC rate. |
| aarch64 | CNTVCT_EL0 | CNTFRQ_EL0 on QEMU virt/cortex-a57; boot establishes CNTVOFF_EL2. |
| arm32 | CNTPCT | CNTFRQ on QEMU virt/cortex-a15; no optional PMU counter or software fallback. |
| rv64, rv32 | Stable high/low/high CLINT `mtime` read at `0x0200bff8` | QEMU virt board contract: 10,000,000 Hz, matching `/cpus/timebase-frequency`. This is not inferred from `rdcycle`. |
| loongarch64 | `rdtime.d` constant counter | CPUCFG4 base frequency multiplied by CPUCFG5 numerator and divided by denominator; missing values fail initialization. |
| mips, mips64 | 32-bit CP0 Count, extended on sampling | Measured between two rising Malta RTC UIP edges. The RTC uses the GT64120 PCI I/O aperture at physical `0x18000000`, accessed through uncached KSEG1. No assumed CPU-frequency divisor. |
| ppc | Stable TBU/TBL/TBU timebase read | QEMU ppce500 board contract: 400,000,000 Hz, matching the CPU node's `timebase-frequency`. Uses the complete 64-bit timebase. |

RISC-V and PPC frequencies were independently checked in device trees emitted
by the installed QEMU 8.2.2 machines using their `dumpdtb` machine property.
These are explicit board contracts rather than runtime FDT discovery. Changing
the board, its timebase or its boot options requires requalifying this contract.
The other sources use an architectural frequency register or boot calibration.
None of this establishes support on arbitrary physical machines, CPU frequency
transitions, cross-CPU counter synchronization, suspend or live migration.

## Conversion and sampling limits

All calls are serialized on the boot CPU. The shared converter retains the
fractional numerator between samples, so repeated sub-nanosecond reads do not
lose time. It splits whole seconds from fractional ticks, checks arithmetic
overflow and leaves state unchanged on failure. Supported frequencies are
nonzero and no greater than `UINT64_MAX / 1000000001`; this bound guarantees
that fractional conversion never needs a freestanding 128-bit division helper.
Elapsed nanoseconds never silently wrap.

CP0 Count must be sampled at least once per complete 32-bit wrap, whose duration
is `2^32 / measured_frequency` seconds. Native preemption and the current polling
idle loop provide regular samples; callers must also sample during long kernel
operations. Multiple unseen wraps cannot be recovered from Count alone. This
backend therefore does not claim arbitrary-duration unobserved idle, stopped
VMs or IRQ-disabled kernel execution. A future always-on wider source or an
independent overflow interrupt is required to remove that limitation. The
64-bit backends have no corresponding short-wrap limitation.

Malta calibration may take almost two seconds. Reference polling is bounded
and missing RTC transitions return an error. PC calibration likewise rejects
a PIT that does not count, and repeats an attempt whose reads may have missed
a wrap. Neither failure falls back to the old nominal timer-period counter.

The PC calibration used to start the TSC after writing a 50 ms mode 0 count
and stop it when it saw OUT2 rise. QEMU starts a mode 0 count when it is
written, whatever the gate, and any stall of the vCPU while it polled OUT2
went straight into the frequency: the guest drift test (WT1.2 slice 6) found
the monotonic clock 502 ppm slow on x86_64 and 686 ppm slow on i386. Bracketed
latch reads at both ends of a longer window remove both errors.

## Validation

`python3 scripts/test_clock_counter.py` exercises initialization, invalid rates,
idempotence, narrow and wide wraps, fractional accumulation and overflow against
an independent wide-integer reference. `--sanitize` enables ASan/UBSan. These are
hosted arithmetic tests, not hardware-clock execution tests.

Compile every backend with the architecture's freestanding flags. Native
acceptance must then run the OS SDK consumer in both modes on all ten profiles,
including a sole sleeping process resuming, clock advancement during repeated
short syscalls, sibling progress and absolute-deadline behavior. MIPS native
runs additionally establish that the RTC reference is reachable; PC runs
establish real PIT calibration. This backend alone does not establish signal
interruption, realtime adjustment, CPU clocks, time64 or complete POSIX P04.

ObfuscationOS `scripts/test_clock_drift.py` (WT1.2 slice 6) measures the
calibration: it brackets 13 guest `CLOCK_MONOTONIC` samples over 120 s with
the host's clock. On the eight profiles whose frequency is a board contract,
an architectural register or the PC's PIT calibration, the drift was within
±6 ppm in every run. mips and mips64 measured between +3 and +22 ppm across
runs, the spread of polling Malta's RTC update edges for one second; every
strict bound stayed within ±60 ppm, inside the test's 100 ppm limit.
Per-profile figures are in [PLATFORM_INTERRUPTS](PLATFORM_INTERRUPTS.md).
Under TCG the counters follow QEMU's virtual clock, so this checks the
frequency the kernel uses, not an oscillator.
