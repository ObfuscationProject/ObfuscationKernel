# Drivers

Drivers derive from `ok::driver::Driver` and are managed by `DriverManager`.
Driver registration is constrained by the `KernelDriver` concept.

Built-in drivers:

- `ConsoleDriver`: appends output to an in-memory console buffer.
- `TimerDriver`: counts timer ticks.
- `NullBlockDriver`: accepts writes and returns zero-filled reads.
- `FramebufferDisplayDriver`: exposes a simple 32-bit RGBA framebuffer with
  clear, pixel, rectangle, and checksum operations for display bring-up tests.
