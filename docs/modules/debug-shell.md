# Debug Shell

`ok::KernelDebugShell` is a kernel-owned command shell for debug builds and
windowed QEMU sessions. It does not provide a separate test executable or hosted
`main`; it is attached during the normal `kernel_main` boot path.

Supported commands:

- `help`: list available commands.
- `status`: print architecture, process, CPU, driver, syscall, and POSIX state.
- `mem`: print frame allocator counters.
- `ps`: print scheduler/process state.
- `drivers`: print registered driver count and input-device status.
- `fs`: exercise a small VFS create/read path.
- `posix`: exercise POSIX open/write/read/stat/close over the RAM VFS.
- `test`: print the last debug-test coverage result.
- `echo <text>`: echo input through the kernel output path.

The shell is intentionally fixed-buffer and freestanding. Windowed QEMU mode
routes keyboard input into the shell after `OK_TEST_PASS`, then mirrors output
to serial and the display driver.
