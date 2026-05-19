# Drivers

Drivers derive from `ok::driver::Driver` and are managed by `DriverManager`.
Driver registration is constrained by the `KernelDriver` concept.

Built-in drivers:

- `ConsoleDriver`: appends output to an in-memory console buffer.
- `TimerDriver`: counts timer ticks.
- `NullBlockDriver`: accepts writes and returns zero-filled reads.
- `FramebufferDisplayDriver`: exposes a simple 32-bit RGBA framebuffer with
  clear, pixel, rectangle, text-line, and checksum operations. Kernel boot writes
  Linux-style startup lines through this driver so `qemu-window-test` can show
  the same debug output that the QEMU checker validates.
