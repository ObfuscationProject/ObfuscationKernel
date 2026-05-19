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
- `KeyboardDriver`: decodes PS/2 set-1 scancodes into key events and ASCII
  characters in polling or interrupt mode.
- `Ps2MouseDriver`: queues PS/2 mouse packets for pointer/button input.

Driver I/O mode is represented by `IoMode` with polling, interrupt, and DMA
variants. The current x86 boot image uses polling for the early keyboard path;
the mode field is already part of `KernelConfig` so interrupt-backed drivers can
be enabled without changing the generic kernel entry.
