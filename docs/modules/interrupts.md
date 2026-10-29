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
profile's `PlatformIrqChip`, selected at compile time. Trap paths run a
bounded top half per line: acknowledge, mask, mark pending, end of interrupt.
The kernel loop drains bottom halves, which run the bound handler and unmask
the line, and it waits for an interrupt when no thread is runnable.
`TrapKind::interrupt` reports a device line that preempted user code. i386
and x86_64 use the 8259A pair. The other profiles keep the model chip and
still poll; see `docs/qualification/PLATFORM_INTERRUPTS.md`.
