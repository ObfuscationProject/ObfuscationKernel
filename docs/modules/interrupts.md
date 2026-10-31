# Interrupts

`ok::interrupt::InterruptDispatcher` owns 256 vectors and dispatches
`ok::arch::TrapFrame` instances to registered handlers.

Handlers can be classes derived from `InterruptHandler` or function objects that
satisfy `InterruptCallable`.

`DispatchMode` records the interrupt path selected by `KernelConfig`:

- `direct`: invoke the registered vector handler immediately.
- `priority_masked`: reserved mode for controller-level priority masking.
- `deferred`: reserved mode for bottom halves or deferred procedure calls.

The baseline registers vector `32` as the timer interrupt and increments the
built-in timer driver.

## Platform interrupt lines

`ok/interrupt/platform.hpp` is the boot-CPU interrupt service over the
profile's `PlatformIrqChip`, selected at compile time: the 8259A pair (i386,
x86_64), GICv2 (arm32, aarch64), the PLIC (rv64, rv32), EIOINTC with PCH-PIC
(loongarch64), Malta's 8259 pair behind the GT-64120 (mips, mips64) and the
MPIC (ppc). Trap paths run a bounded top half per line: acknowledge, mask,
mark pending, end of interrupt. The kernel loop drains bottom halves, which
run the bound handlers (up to four per line) and unmask the line, and it
waits for an interrupt when no thread is runnable and no device event is left
to serve. `TrapKind::interrupt` reports a device line that preempted user
code. `virtio_mmio_line()` and `pci_intx_line()` give drivers their board's
line. The native runtime initializes its backend (the trap vectors) and
starts the service before it mounts the root, so the root's disk and the
entropy source complete through their lines from the first request.

`count_device_wait()` records how a driver's wait for its device completed
(after the device's interrupt, or polled), and `record_timer_latency()` how
long after its programmed expiry each timer interrupt was observed, for the
idle wait's tick and the tick that preempts user code
(`time::LatencyHistogram`, `ok/time/latency_histogram.hpp`). The interrupt
report prints both as `OK_IRQ_COMPLETIONS` and `OK_TIMER_LATENCY`.

## Device event sources

`EventSource` names what the kernel loop serves for devices: the console, the
kernel's input devices and the network link. A driver whose line is bound
calls `drive_events()` once; its bottom half then posts the source and the
loop serves it only after a post. A consumer that stops before the source is
empty (a one-byte console step, the Ethernet receive budget) posts it again.
An undriven source, before the service starts or without a bound line, is
polled on every pass. The UART receive rings are one
`ConsoleRing` (`src/interrupt/console_ring.hpp`) on every profile: the
handler fills it, and the console is read outside the handler only to refill
a ring that overflowed.

`sched::WaitRegistration` records the `WakeSource` that completed a wait:
another thread, a timer (every timeout), an interrupt-driven device event, or
a polled device. Console reads, poll/select on the console or a socket, TCP
waits and UDP receives made ready by device data take the source of that
device's events (`event_wake_source()`); the scheduler counts ready and timed
out waits by source. `OK_IRQ_EVENTS` reports each event source as
`driven:posted:served:polled` and the wake counts; see
`docs/qualification/PLATFORM_INTERRUPTS.md`.

## Interval timers

Linux processes' `ITIMER_REAL` and POSIX timers (`src/user/runtime/timers.cpp`)
expire in the runtime step, so the tick that ends an idle wait also serves
them; their signals use the pending-signal queues and rearm on dequeue. See
[Linux interval timers](../abi/linux-timers.md).
