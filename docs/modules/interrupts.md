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

`ok/interrupt/platform.hpp` is the platform interrupt service over the
profile's `PlatformIrqChip`, selected at compile time: the 8259A pair (i386),
the I/O APIC with the 8259A pair as fallback (x86_64), GICv2 (arm32, aarch64), the PLIC (rv64, rv32), EIOINTC with PCH-PIC
(loongarch64), Malta's 8259 pair behind the GT-64120 (mips, mips64) and the
MPIC (ppc). Trap paths run a bounded top half per line: acknowledge, mask,
mark pending, end of interrupt. Bottom halves run the bound handlers (up to
four per line) and unmask the line; the kernel loop drains them and waits for
an interrupt when no thread is runnable and no device event is left to serve.
`TrapKind::interrupt` reports a device line that preempted user code.
`virtio_mmio_line()` and `pci_intx_line()` give drivers their board's line.
The native runtime initializes its backend (the trap vectors) and starts the
service before it mounts the root, so the root's disk and the entropy source
complete through their lines from the first request.

### Several CPUs

`IrqRouter` (`ok/interrupt/irq_chip.hpp`) keeps its line records, handler
lists, statistics and every controller read-modify-write sequence (GICD_ICFGR,
the PLIC enable words and thresholds, the 8259 mask shadow, the I/O APIC's
index/data register pair) under one
`irq_router` lock, taken with interrupts masked and never held while a
handler runs. A drain claims a line's pending flag by atomic exchange, so one
CPU at a time runs that line's handlers, and the line stays masked from its
top half until they returned. Bottom halves still run under the kernel lock:
the boot CPU's loop, a secondary CPU's runtime step and a driver waiting
inside a syscall drain them. The boot CPU alone consumes the event sources;
a post made on another CPU that finds its source idle sends CPU 0 an IPI
(`ipi_device`), so its idle wait ends.

On controllers that can target CPUs (`RoutableIrqChip`: GICv2 through
GICD_ITARGETSR, one target per line; the PLIC through hart h's M-mode context
2h; the I/O APIC through each redirection entry's physical destination) a
line bound while N CPUs are online goes to the (line mod N)-th online CPU in
logical order. aarch64, rv64 and x86_64 route this way; arm32 and rv32 run
one CPU, and i386 keeps the 8259 on CPU 0. `retarget_platform_irq()` moves
one bound line. `retarget_platform_irqs_away(cpu)` prepares CPU hotplug: it
decides and moves every line delivered to `cpu` under the router lock
(`IrqRouter::retarget_away()`), spreading them over the other online CPUs.
Called on `cpu` itself with interrupts masked, it then serves what that CPU's
controller interface already accepted (a vector in the local APIC's IRR,
ISR_EL1.I, mip.MEIP) so its top half runs and completes there; an x86 level
line would otherwise stay blocked behind its Remote IRR. A pending,
unclaimed GIC or PLIC interrupt instead follows its new target. No offline
path calls it yet; the native probes exercise it.
Secondary CPUs run top halves from their user trap path, their idle wait and
while they spin for the kernel lock (`lock_kernel_serving_interrupts()`,
`serve_local_interrupts()`): a lock holder may be waiting for a line routed to
the spinning CPU.

`wait_for_interrupt()` halts the calling CPU with interrupts masked until it
takes a device line, a tick (a secondary CPU's local timer) or an IPI. Each
CPU has its own wake flag. A device waiter publishes that it waits before it
checks for pending lines; a top half on another CPU marks its line pending
before it reads who waits and sends each waiter an IPI, which stays pending at
the waiter's controller until its halt claims it, so no wake is lost. On
aarch64, rv64 and x86_64 (on the I/O APIC) a driver waiting inside a
syscall on a secondary CPU halts there instead of polling; it keeps the kernel
lock while it waits. An x86_64 boot left on the 8259 fallback polls there. The boot CPU's kernel loop idles in
`idle_until_interrupt()`, which top halves on other CPUs do not end: a
secondary CPU drains the lines it took in its next runtime step (before the
runtime admits it, whenever the kernel lock is free), and the event posts of
those bottom halves reach CPU 0 as IPIs.

`count_device_wait()` records how a driver waited for its device (on its
bound line, rechecking only after its own interrupt, or polled), and `record_timer_latency()` how
long after its programmed expiry each timer interrupt was observed, for the
idle wait's tick and the tick that preempts user code
(`time::LatencyHistogram`, `ok/time/latency_histogram.hpp`). The interrupt
report prints both as `OK_IRQ_COMPLETIONS` and `OK_TIMER_LATENCY`. With
secondary CPUs started (a CPU parked by hotplug still counts) it ends with `OK_SMP_IRQS scope=hardware
cpuK=<top halves run>:<lines drained>:<interrupt waits> ... routes=<line>@<cpu>,...`.

### x86_64: the I/O APIC

`arch::x86::PcIrqChip` (`src/arch/x86_64/ioapic.hpp`) is x86_64's chip. When
`smp::discover()` finds the xAPIC page `boot.S` maps, it keeps the MADT's I/O
APIC entries and ISA interrupt source overrides (`boot::MadtInterrupts`,
`ok/boot/acpi.hpp`), on uniprocessor boots too. A malformed one (a short
entry, no address, a duplicate I/O APIC, an override of a bus other than
ISA, with reserved flags, of one IRQ twice or of two IRQs onto one GSI)
marks the description `rejected` and empty: the CPU map still parses, and the
chip keeps the 8259 pair. `initialize()` then picks the
I/O APIC whose GSIs start at 0, inside the uncached 2 MiB at 0xfec00000 that
`boot.S` maps, and masks every redirection entry. It masks both 8259s and the
boot CPU's LINT0 (no ExtINT any more), enables the boot CPU's local APIC and
delivers the PIT on the GSI the MADT moves ISA IRQ 0 to (GSI 2 on QEMU),
always to the boot CPU. Without such an I/O APIC it keeps the 8259 pair on
the boot CPU, as i386 does.

Line N is GSI N on vector 32 + N (`execution.S` has stubs for vectors
32..63, so 32 GSIs). Entries use fixed delivery and a physical xAPIC id;
trigger mode comes from the binding, polarity from the MADT (an override,
else ISA active high, else PCI active low). The CPU that took the vector
completes it with a local APIC EOI, which also clears a level line's Remote
IRR. `isa_irq_line()` (`src/interrupt/isa_irq.hpp`) and `pci_intx_line()`
translate an ISA IRQ through the overrides. The user trap path and the
kernel's waits (`halt_until_interrupt()`, `wait_local()` and the
`sti; nop; cli` window of `serve_local_interrupts()`) run top halves on any
CPU; a secondary's local timer in a kernel wait ends that wait.

The I/O APIC drops an edge that arrives while its entry is masked, so the
router leaves edge-triggered lines unmasked in the top half on this chip
(`loses_masked_edges()`): an edge during the bottom half makes the line
pending again and its handlers run once more. Level-triggered lines are
masked as on the other chips.

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
