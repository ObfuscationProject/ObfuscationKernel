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
