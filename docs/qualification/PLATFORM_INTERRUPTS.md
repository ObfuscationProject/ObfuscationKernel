# Platform interrupts and the idle wait

WT1.2 `feat/irq-wait-time` (Kernel issue #4), slice 1: the interrupt core and
the first controller. This note grows with each slice.

## Design

- `IrqRouter<Chip>` (`include/ok/interrupt/irq_chip.hpp`) splits an interrupt.
  The top half runs in the trap path with interrupts disabled. It
  acknowledges the line, masks it, records it pending and completes it at the
  controller, so a level-triggered device cannot re-enter before it is
  serviced. `drain()` runs from the kernel loop. It calls each pending line's
  bottom-half handler once, with the line still masked, then unmasks it.
  Trap context runs no device code and allocates nothing.
- `ok/interrupt/platform.hpp` is the boot-CPU service over one
  `PlatformIrqChip`. The chip is chosen at compile time per profile in
  `src/interrupt/platform_irq.cpp`, so it is static dispatch with no virtual
  call on the interrupt path. The service provides
  `start_platform_irqs()`, `bind_platform_irq()`, `drain_platform_irqs()`,
  `wait_for_interrupt()` and statistics. Profiles without a controller driver
  keep the model chip; there `wait_for_interrupt()` returns false and the loop
  polls as before.
- The kernel loop (`src/core/kernel_main.cpp`) starts the service once native
  userland runs, because the trap vectors belong to the native backend. It
  drains bottom halves on every iteration. When nothing was polled and the
  runtime found no thread to run (`user::native::idle()`), it waits for an
  interrupt instead of spinning. The tick that preempts user code also ends
  the wait, so timed waits are checked at least once per tick.
- `TrapKind::interrupt`: a bound device line that preempts user code arrives
  after its top half has run. The thread resumes like a timer preemption but
  is not charged a CPU tick. Native captures require a privileged stack for
  it, as they do for the timer.
- `OK_IRQ_PLATFORM` follows the `OK_ENTRY_STACKS` report once the service
  runs. It reports waits, ticks, deliveries, serviced bottom halves,
  user-mode interrupts, spurious lines and controller errors. A clean
  shutdown prints the same counters on one line before `OK_SHUTDOWN`.

## Controllers

| Profiles | Controller | Status |
| --- | --- | --- |
| i386, x86_64 | 8259A pair (`src/arch/x86_common/pic8259.hpp`): vectors 32–47, ELCR edge/level, spurious 7/15 detection | Done. The PIT tick (line 0) wakes the wait; COM1 receive is bound to line 4 and fills a 256-byte ring that the console poll drains first |
| arm32, aarch64 | GICv2 (`src/arch/arm_common/gicv2.hpp`): SPIs 32–255, ICFGR edge/level, CPU 0 targets | Done. The kernel waits with CPSR.I set (WFI wakes on a pending IRQ regardless) and claims from GICC_IAR; a one-shot physical-timer PPI 30 at 100 Hz ends the wait. The user IRQ path completes only the timer PPI and reports other IDs as `TrapKind::interrupt`. PL011 receive is bound to ID 33 (SPI 1, level) |
| rv64, rv32 | PLIC | Slice 2 |
| loongarch64 | EIOINTC with PCH-PIC | Slice 2 |
| mips, mips64 | Malta i8259 behind the GT-64120 | Slice 2 |
| ppc | MPIC | Slice 2 |

## Results (x86_64, QEMU 10.2.0, ObfuscationOS main with BusyBox)

- Hosted (`scope=model`): `scripts/test_irq_wait_time.py`. It covers bind and
  unbind, a top half that acknowledges, masks, records the line pending and
  completes it, coalescing of repeated top halves, a bottom half that runs
  with the line still masked, and unclaimed lines masked and completed as
  spurious.
- `xmake test` passes on all ten profiles.
- `qemu-distro --fs=ext4 --require-native` and `scripts/test_musl_shell.py`
  pass on x86_64: 35 sh checks, and 133/151 suite cases with the 9 reviewed
  failures, the same as kernel main. The shell run takes 69–70 s against
  70.7 s on main.
- A guest `sleep 2` takes 2.05 s of host time. During it the loop idles about
  200 times, one per tick: `waits=240 ticks=333` after boot and the sleep.
- During the shell test COM1 delivered 14 interrupts, 12 of them while user
  code ran (`TrapKind::interrupt`), with no spurious line or controller
  error.

## Results (arm32 and aarch64, QEMU 10.2.0, ObfuscationOS main with BusyBox)

- `xmake test`, `qemu-distro --fs=ext4 --require-native` and
  `scripts/test_musl_shell.py` pass: 35 sh checks, and 133/151 suite cases
  with the 9 reviewed failures. The PL011 delivered 166 interrupts, all while
  user code ran.
- A guest `sleep 2` takes 2.08 s of host time over 165 idle waits.
- A user-mode shell loop with no syscalls still takes timer preemptions after
  the service starts: 199 (`timers=199`, `ticks=365` with the idle ticks).
- aarch64: the same shell results; the PL011 delivered 120 interrupts in user
  mode; `sleep 2` takes 2.08 s; the shell loop takes 152 timer preemptions
  (`waits=169 ticks=321`).

## Limits

- The console poll still reads the UART when the ring is empty, and network,
  block and input are still polled. The wait only replaces the idle spin.
- Wait granularity is the 100 Hz PIT tick; no one-shot deadline programming.
- No local APIC or I/O APIC; the 8259 only.
