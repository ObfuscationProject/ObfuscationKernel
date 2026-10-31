# Platform interrupts and the idle wait

WT1.2 `feat/irq-wait-time` (Kernel issue #4). Slices 1 and 2: the interrupt
core and one controller per profile. Slice 3: device completion through
interrupts. Slice 4: event sources and wake sources, so that waits are woken
from interrupts and no device is polled. Slice 5: Linux interval timers. This
note grows with each slice.

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
  user-mode interrupts, spurious lines and controller errors, and
  `OK_IRQ_EVENTS` (slice 4) follows it. A clean shutdown prints both lines
  before `OK_SHUTDOWN`.

## Controllers

| Profiles | Controller | Status |
| --- | --- | --- |
| i386, x86_64 | 8259A pair (`src/interrupt/pic8259.hpp` over x86 port I/O): vectors 32–47, ELCR edge/level, spurious 7/15 detection | Done. The PIT tick (line 0) wakes the wait; COM1 receive is bound to line 4 and fills a 256-byte ring that the console poll drains first |
| arm32, aarch64 | GICv2 (`src/arch/arm_common/gicv2.hpp`): SPIs 32–255, ICFGR edge/level, CPU 0 targets | Done. The kernel waits with CPSR.I set (WFI wakes on a pending IRQ regardless) and claims from GICC_IAR; a one-shot physical-timer PPI 30 at 100 Hz ends the wait. The user IRQ path completes only the timer PPI and reports other IDs as `TrapKind::interrupt`. PL011 receive is bound to ID 33 (SPI 1, level) |
| rv64, rv32 | PLIC (`src/arch/riscv_common/plic.hpp`), hart 0 machine context: sources 1–127 | Done. The M-mode kernel waits with mstatus.MIE clear (WFI wakes on `mip & mie`) and a one-shot CLINT compare at 100 Hz, then claims from the PLIC. User mode enables MEIE once the service runs; the trap assembly separates the machine timer (cause 7) from device interrupts. NS16550 receive is bound to source 10 |
| loongarch64 | EIOINTC with PCH-PIC (`src/arch/loongarch64/eiointc.hpp`): PCH-PIC inputs 0–31 forwarded to the same EIOINTC vectors, core 0, IP0 | Done. Masking clears the EIOINTC enable bit, unmasking re-delivers an asserted input; the core ISR is the claim and its write-one-to-clear the completion. The kernel idles with CRMD.IE clear (IDLE wakes on `ESTAT.IS & ECFG.LIE`) and a one-shot constant timer. User mode enables IP0 once the service runs. The UART is bound to PCH-PIC input 2 |
| mips, mips64 | Malta's PIIX4 8259 pair (`Pic8259<MaltaPortIo, false>`) through the GT-64120 PCI I/O window, on CPU IP2 | Done. These pre-R6 cores leave WAIT only for an interrupt they can take, so `ok_mips_idle` sets IE with IM2 and IM7; the exception vector recognizes the idle window, resumes after WAIT with IE clear, and the C side claims the line with an OCW3 poll. The CP0 timer is the tick, so 8259 line 0 stays masked. User mode adds IM2 once the service runs; the o32/n64 capture readiness ignores that kernel-owned bit. COM1 receive is bound to line 4 |
| ppc | Freescale MPIC (`src/arch/ppc/mpic.hpp`): sources 0–255 programmed with their own number as the vector, routed to CPU 0 | Done. e500 dozes on MSR[WE] with HID0[DOZE], but QEMU's BookE model wakes only for an interrupt MSR[EE] admits, so `ok_ppc_idle` sets EE for a short window and the IVOR4/IVOR10 vectors return past it with EE clear; the wait then claims with IACK. A one-shot decrementer is the tick. User-mode IVOR4 returns as `TrapKind::interrupt`; the private PPC readiness treats it like the decrementer (trap 0x500). The DUART is bound to source 42 |

## Ten-profile result

After merging main (module ABI 77), all ten profiles pass `xmake test`, the
ext4 native boot and `scripts/test_musl_shell.py` against ObfuscationOS main
`ddbfe45`: 54 sh checks, and 134/151 suite cases with the 8 reviewed failures,
the same as kernel main. Every profile idles on interrupts and receives its
console UART through the interrupt, with no spurious line or controller
error. The per-profile sections below give the first runs.

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

## Results (rv64 and rv32, QEMU 10.2.0, ObfuscationOS main with BusyBox)

- `xmake test`, the ext4 native boot and `scripts/test_musl_shell.py` pass on
  both: 35 sh checks, and 133/151 suite cases with the 9 reviewed failures.
  The UART delivered 165 (rv64) and 166 (rv32) interrupts, all in user mode.
- `sleep 2` takes 2.07–2.08 s of host time. The syscall-free shell loop takes
  225 (rv64) and 217 (rv32) timer preemptions after the service starts.

## Results (loongarch64, QEMU 10.2.0, ObfuscationOS feat/linux-personality with BusyBox, ABI 77)

- `xmake test`, the ext4 native boot and the shell test pass: 54 sh checks, and
  134/151 suite cases with the 8 reviewed failures, as on main. The UART
  delivered 167 interrupts, 165 of them in user mode.
- `sleep 2` takes 2.08 s of host time. The shell loop takes 153 timer
  preemptions (`waits=179 ticks=332`).

## Results (mips and mips64, QEMU 10.2.0, ObfuscationOS feat/linux-personality with BusyBox, ABI 77)

- `xmake test`, the ext4 native boot and the shell test pass on both: 54 sh
  checks, and 134/151 suite cases with the 8 reviewed failures. COM1 delivered
  166 interrupts each, 164 of them in user mode.
- `sleep 2` takes 2.10–2.11 s of host time. The shell loop takes 308 (mips)
  and 312 (mips64) timer preemptions.

## Results (ppc, QEMU 10.2.0, ObfuscationOS feat/linux-personality with BusyBox, ABI 77)

- `xmake test`, the ext4 native boot and the shell test pass: 54 sh checks, and
  134/151 suite cases with the 8 reviewed failures. The DUART delivered 169
  interrupts, 167 of them in user mode.
- `sleep 2` takes 2.23 s of host time. The shell loop takes 52 s here and
  52.9 s on kernel main (4,862 and 4,906 timer preemptions): ppc is slow on
  that loop either way, so this is no regression.

## Slice 3: device completion through interrupts

Board routing (`ok/interrupt/platform.hpp`), for drivers in the OS image:

| Boards | Call | Line |
| --- | --- | --- |
| QEMU virt, arm32 and aarch64 | `virtio_mmio_line(slot)` | GIC ID 48 + slot, rising edge |
| QEMU virt, rv64 and rv32 | `virtio_mmio_line(slot)` | PLIC source 1 + slot |
| PC (i386, x86_64) | `pci_intx_line(device, pin, line_register)` | The 8259 line SeaBIOS writes into 0x3c |
| loongarch64 virt | `pci_intx_line` | PCH-PIC input 16 + (slot + pin - 1) % 4 (GPEX swizzle) |
| Malta (mips, mips64) | `pci_intx_line` | PIIX4 PIRQ by slot, routed by the `-kernel` bootloader to 8259 lines 10, 10, 11, 11 |
| ppce500 | `pci_intx_line` | MPIC source 1 + (slot + pin - 1) % 4 |

- A router line holds up to four handlers with one trigger (PCI INTx is
  shared). `OK_IRQ_PLATFORM` lists bound lines as `line:delivered`.
- ObfuscationOS `feat/irq-wait-time`: the virtio block, net and RNG drivers
  bind their line at the first request after the service starts. They clear
  `VIRTQ_AVAIL_F_NO_INTERRUPT` and, on PCI, the INTx disable bit, and the
  bottom half acknowledges the device. A request then sleeps in
  `wait_for_interrupt()` and drains bottom halves between readiness checks.
  Before the service starts (the root mounts first), or when no line can be
  bound, requests are polled as before.
- Kernel input: the virtio-mmio keyboard and mouse (arm32, aarch64, rv64,
  rv32) and the PS/2 8042 (i386, x86_64, lines 1 and 12) are bound. The main
  loop reads their queues or the controller only after an interrupt, until
  they are drained. mips, mips64 and ppc have no input device.
- Interrupt cost: per completion the 8259 writes only the mask register of
  the chip that holds the line. Malta claims with one GT-64120 `GT_PCI0_IACK`
  read (the byte-swapped vector) instead of a four-access OCW3 poll. A stale
  cascade from a request that dropped between the polls is completed;
  otherwise its in-service bit blocks every later slave line.

Results on all ten profiles (QEMU 10.2.0, ObfuscationOS `feat/irq-wait-time`
on main `ddbfe45`, ABI 77):

- `xmake test`, the ext4 native boot and the shell gate pass: 54 sh checks,
  and 134/151 suite cases with the 8 reviewed failures.
- The disk completed through its line on every profile, about 1.2–1.6 million
  interrupts per gate run (3.0 million on ppc), and the RNG on its line.
  There were no controller errors. The only spurious interrupts were 8259 ones
  (up to 56 on x86, 8 on Malta), from a level line that dropped before its
  acknowledge cycle.
- `scripts/test_native_udp_socket.py` passes on x86_64 with the NIC bound.
- QMP `send-key` of five keys arrives as ten interrupts on the keyboard's
  line on x86_64, i386, arm32, aarch64, rv64 and rv32.
- Cost, same kernel SDK, idle host, two runs each: the x86_64 shell gate takes
  110.3 and 111.6 s with OS main's polled drivers and 110.7 and 111.2 s with
  interrupt completion; mips takes 171.5 and 172.1 s polled and 166.7 and
  168.7 s with interrupts.

## Slice 4: event sources and wake sources

- `EventSource` (`ok/interrupt/platform.hpp`): the console, the kernel's
  input devices and the network link. A driver whose line is bound calls
  `drive_events()`; its bottom half posts the source, and the kernel loop
  serves a driven source only after a post (`take_event()`). A consumer that
  stops early posts again: the console after each byte while bytes remain,
  the input devices until their queues or the 8042 are empty, the Ethernet
  executor when it stopped at its eight-frame receive budget. An undriven
  source is polled on every pass. The loop does not idle while a post is
  pending.
- Console: one `ConsoleRing` (`src/interrupt/console_ring.hpp`) replaces the
  ten per-platform rings. Once the console is driven the UART is read only by
  its bottom half; outside it only to refill a ring that overflowed, because
  an edge line does not fire again for bytes already in the FIFO. Before,
  every loop pass read the line status register when the ring was empty.
- Input: the PS/2 and virtio-mmio handlers post the input source. Profiles
  without an input device (mips, mips64, ppc, and loongarch64 when its
  virtio-pci devices are absent) drive the source with nothing to poll.
- Network: the NIC's bottom half (ObfuscationOS `feat/irq-wait-time`) posts
  the network source after `drive_events()` at bind time. A pass without a
  post (`net::LinkPass{device = false}`) leaves the TX ledger and the RX queue
  alone and runs only closes, TCP timers and queued segments with the TX
  credit it holds. The first pass, before the driver binds its line, polls.
- Block and RNG (ObfuscationOS): the bottom half also signals the waiting
  request, which rechecks the used ring only after its own line fired, not on
  ticks or other lines. With the line bound only the bottom half reads the
  virtio ISR. A request that found its completion before its interrupt was
  taken used to read the ISR itself, which dropped INTx under the pending
  interrupt; the 8259 then delivered it as spurious (41,536 times in one
  x86_64 gate run on a loaded host, 0 after the change).
- No poll behind the console also means none to hide a lost interrupt. Two
  appeared on the virt boards and are fixed. On arm32 and aarch64 the PL011
  handler wrote UARTICR after draining DR: a byte that arrived in between had
  its RX interrupt cleared, and the PL011 raises RX again only when its FIFO
  goes from empty to one byte, so input stopped after the first byte. Reading
  DR to empty already clears RX and the receive timeout; the handler no longer
  writes UARTICR. On rv64 and rv32 QEMU's PLIC re-evaluates its outputs on a
  threshold write but not on an enable write: a UART byte that arrived while
  the router had source 10 masked stayed pending, never reached MEIP after the
  unmask, and the 16550, holding that byte, never raised its line again.
  `Plic::unmask()` rewrites the threshold.
- Wake sources: `sched::WaitRegistration` records the `WakeSource` that
  completed a wait (thread, timer, interrupt, poll); every timeout is a timer
  wake. A console read woken by input, poll/select made ready by console
  input or socket data, a TCP wait made readable, writable or connected, and
  a UDP receive made ready by a queued datagram take the source of that
  device's events: interrupt once it is driven, poll before. The scheduler
  counts ready and timed-out waits by source.
- `OK_IRQ_EVENTS scope=hardware console=D:P:S:Q input=… network=…
  wakes=thread:…,timer:…,interrupt:…,poll:…` gives, per source, driven (1/0),
  posts, served posts and polled passes.

Results (QEMU 10.2.0, ObfuscationOS `feat/irq-wait-time`, ABI 77):

- Hosted (`scope=model`): `scripts/test_irq_wait_time.py` adds the wake
  source of a registration, undriven, driven and independent event sources,
  and the console ring (no UART access while driven, a burst larger than the
  ring refilled from the FIFO). `tests/host/tcp_streams_tests.cpp` adds a pass
  without a device event (no reap or receive), the interrupt source credited
  to a TCP waiter its SYN-ACK made ready, and the receive backlog.
  `tests/host/native_console_lease_tests.cpp` checks a blocked console read
  woken with the interrupt source. ObfuscationOS
  `tests/drivers/virtio_poll_tests.cpp` checks that unsignalled wakes do not
  recheck the device.
- All ten profiles: `xmake test`, the ext4 native boot and the shell gate pass
  (54 sh checks, 134/151 with the 8 reviewed failures). On every profile the
  console is driven and never polled (`console=1:…:172:0` over the 172 typed
  bytes), and no interrupt is spurious or a controller error. The disk
  completes through its line, 1.18–1.58 million interrupts per run (3.03
  million on ppc).
- x86_64 on an idle host: the shell gate takes 111.4 s (110.7–111.2 s in
  slice 3), with `delivered=serviced=1262207`.
- Console wakes: the harness types each line as soon as `oksh> ` appears. On
  x86 the shell is already blocked in `read()`, so each line is an interrupt
  wake (three for the probe's three commands). On the slower profiles the
  bytes arrive while the shell is still running (`user_interrupts` equals the
  bytes typed) and its read never sleeps.
- Input: QMP `send-key` of five keys gives ten interrupts on the keyboard's
  line on x86_64, i386, arm32, aarch64, rv64 and rv32; the input source is
  driven and served after each (`input=1:22:22:0` on x86, `1:16:16:0` on the
  virt boards), never polled. On loongarch64 it stays undriven and polled
  (`input=0:0:0:95667` over a gate run); mips, mips64 and ppc have no input
  device (`input=1:0:0:0`).
- x86_64 `scripts/test_native_udp_socket.py` (ObfuscationOS) passes with
  `network=1:7:7:1`: one polled pass before the driver bound its line, then
  seven passes, one per NIC interrupt. The probe's child now receives on a
  blocking socket, and its wait is the run's interrupt wake.
- ObfuscationOS artifact tests: 1321 run, OK (34 skipped).

## Slice 5: interval timers

[Linux interval timers](../abi/linux-timers.md) gives the contract.

- Calls: `setitimer`, `getitimer` and `alarm` (`ITIMER_REAL`), and
  `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun` and
  `timer_delete` with their time32/time64 forms, generated and crosschecked
  against Linux 6.13.4 on ten profiles (`alarm` on the five that have it).
- `time::IntervalTimer` (`ok/time/interval_timer.hpp`) keeps the Linux 6.13
  contract: one queued signal per expiry, an interval timer rearms when that
  signal is dequeued, and the intervals it missed are the signal's overrun.
- Signal records: `PendingSignalInfo` carries the timer id, overrun and
  sigevent value; `SI_TIMER` is -2, -3 on MIPS. The siginfo codec and every
  frame backend write `si_tid`, `si_overrun` and `si_value`; `SIGALRM` as
  `SI_KERNEL`, which is how Linux delivers an `ITIMER_REAL` expiry, now
  reaches handler frames.
- Service: each runtime step reads the clocks, expires due timers before
  interruptible waits are chosen, queues their signals with the `kill` and
  `tgkill` admission, keeps a queued record's overrun current and rearms a
  timer whose record left its queue. The idle wait needs nothing new: the
  tick ends it and the next step expires the timer, so granularity is the
  100 Hz tick.
- The guest probe found that every backend refused to resume a thread whose
  stack pointer was unmapped, and the runtime then ended its process. A
  detached musl thread, such as the `SIGEV_THREAD` helper, unmaps its stack
  and calls `exit` without touching it, so its exit killed the process.
  Entry no longer requires a mapped stack, nor on MIPS an 8-byte aligned one
  (musl's o32 exit path points `sp` at its code); an access through it still
  faults.
- `OK_TIMERS scope=hardware armed=… expired=… signalled=… rearmed=…
  retried=…` follows `OK_IRQ_EVENTS` once a timer was armed.

Results (QEMU 10.2.0, ObfuscationOS `feat/irq-wait-time`, ABI 77):

- Hosted (`scope=model`): `scripts/test_irq_wait_time.py` adds the timer
  model (expiry, dequeue rearm, overrun, forwarding, saturation), the wire
  records of four profiles, the queue records and `SI_TIMER` siginfo.
  `scripts/test_linux_timers.py` runs the calls through the actual runtime
  with hosted copies and a model clock (220 checks): `ITIMER_REAL` one-shot,
  periodic while blocked and ignored, `alarm` rounding, ids and refused
  sigevents, an x86_64 SA_SIGINFO frame with `SI_TIMER`, overrun across a
  dequeue, reprogramming and deletion of a queued record, `SIGEV_NONE`,
  `SIGEV_THREAD_ID`, an owed expiry, a parked ignored timer, `CLOCK_REALTIME`
  absolute and relative, exec and retirement. Both pass with sanitizers.
  The hosted suite passes with the toolchain runners (142 runners), whose
  native execution probes now enter with an unmapped stack.
- All ten profiles: `xmake test`, the ext4 native boot, the shell gate
  (54 sh checks, 134/151 with the 8 reviewed failures) and the ObfuscationOS
  guest probe `tests/musl/linux_timers.c` pass. The probe checks `alarm`,
  one-shot and periodic `ITIMER_REAL` with `SI_KERNEL`, `EINTR` from
  `nanosleep`, `SIGEV_SIGNAL` with `si_timerid` and `si_value`, the overrun of
  a periodic 10 ms timer blocked for 105 ms and `timer_getoverrun`,
  `SIGEV_NONE`, musl's `SIGEV_THREAD`, a `TIMER_ABSTIME` `CLOCK_REALTIME`
  expiry, and the fork and exec rules. ObfuscationOS artifact tests: 1321
  run, OK (34 skipped).

Guest probe lines (`TIMERS_PASS`): the one-shot `ITIMER_REAL` handler ran
3.6–12.4 ms after its 50 ms expiry, as the tick serves it; the blocked 10 ms
timer reported 9 or 10 overruns over 105 ms; musl's `SIGEV_THREAD` ran its
function 3 or 4 times before the timer was deleted. Every profile reports
`OK_TIMERS armed=12 expired=14 signalled=14 rearmed=9 retried=0` (15/15/10
when a fourth `SIGEV_THREAD` expiry fit in). The shell gate ran beside a
second lane and the hosted suite, so its times are not comparable with slice 4;
rerun on x86_64 without them it takes 113.1 s (111.4 s in slice 4).

| Profile | Probe checks | One-shot late | Overruns | Shell gate |
| --- | --- | --- | --- | --- |
| i386 | 74 | 12.4 ms | 9 | 133.0 s |
| x86_64 | 74 | 3.6 ms | 9 | 197.7 s |
| aarch64 | 75 | 11.8 ms | 9 | 140.6 s |
| arm32 | 74 | 4.9 ms | 10 | 141.6 s |
| rv64 | 75 | 10.6 ms | 9 | 131.3 s |
| rv32 | 75 | 3.8 ms | 10 | 137.2 s |
| loongarch64 | 75 | 8.9 ms | 10 | 123.0 s |
| mips | 74 | 7.0 ms | 9 | 177.7 s |
| mips64 | 74 | 9.8 ms | 9 | 233.8 s |
| ppc | 74 | 8.0 ms | 10 | 325.8 s |

The probe count differs by one where the `SIGEV_THREAD` wait looped once more.
On every profile no interrupt was spurious or a controller error. Before the
entry fix the probe died on every profile when the `SIGEV_THREAD` helper
exited (`enter=failed`), and on mips, after the first fix, for the misaligned
`sp` musl's o32 exit path sets.

## Limits

- loongarch64's virtio-pci keyboard and mouse stay polled. Bound, the
  keyboard's INTx (PCH-PIC input 18) kept re-asserting after each ISR read
  and the guest made no progress; the cause is not found yet.
- The runtime still rechecks its software waits (poll/select, UDP receives,
  futex and sleep deadlines) on every step after any wake. The device behind
  them is touched only after its interrupt, but the waits are not yet linked
  to the event that readied them.
- Wake attribution is by readiness kind: a socket wait made ready by queued
  data is credited to the link even if the data arrived before it slept.
- Wait and timer granularity is the 100 Hz tick; no one-shot deadline
  programming.
- `ITIMER_VIRTUAL`, `ITIMER_PROF` and CPU-time clocks are refused: there is
  no CPU-time accounting. Other limits of the timer calls are in
  [Linux interval timers](../abi/linux-timers.md).
- No local APIC or I/O APIC; the 8259 only.
