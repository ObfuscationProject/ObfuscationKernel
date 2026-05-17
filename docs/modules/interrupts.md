# Interrupts

`ok::interrupt::InterruptDispatcher` owns 256 vectors and dispatches
`ok::arch::TrapFrame` instances to registered handlers.

Handlers can be classes derived from `InterruptHandler` or function objects that
satisfy `InterruptCallable`.

The baseline registers vector `32` as the timer interrupt and increments the
built-in timer driver.

