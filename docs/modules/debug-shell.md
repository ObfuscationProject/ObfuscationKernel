# Debug Shell

`ok::KernelDebugShell` is a kernel-owned command shell for debug builds and
windowed QEMU sessions. It does not provide a separate test executable or hosted
`main`; it is attached during the normal `kernel_main` boot path.

The interactive line editor keeps the prompt outside the editable buffer:
Backspace/Delete only erase user input, and Ctrl-U clears the current input
line. The command evaluator follows the Bourne shell subset needed for kernel
debug work: comments beginning with `#`, command sequences separated by `;`,
and basic `&&`/`||` conditionals.

Supported commands:

- `help`, `true`, `false`, `:`, `clear`, `uname`: basic shell commands.
- `status`: print architecture, process, CPU, driver, syscall, and POSIX state.
- `mem`: print frame allocator counters.
- `ps`: print scheduler/process state.
- `drivers`: print registered driver count and input-device status.
- `fs`: print RAM VFS state for `/tmp/kernel.log`.
- `posix`: print POSIX process, cwd, and open-file state.
- `test`: print the last debug-test coverage result.
- `echo <text>`: echo input through the kernel output path.
- `pwd`, `cd <path>`, `ls [path]`, `cat <path>`, `touch <path>`,
  `mkdir <path>`, `rm <path>`, `stat <path>`: BusyBox-style file commands over
  the RAM VFS/POSIX layer.
- `whoami`, `id`, `su kernel|root|user`: switch the debug shell session between
  the initial kernel context, root, and a normal test user.
- `disk`: print active block-device geometry and SimpleFS mount state.
- `mkfs [label]`: format the active block device as SimpleFS.
- `sfs info|ls|touch|write|cat|stat|rm`: operate directly on the SimpleFS flat
  root directory for block-filesystem testing.
- `ext4 status|disk`: report EXT4 parser/block-mount state or try mounting the
  current block device as EXT4.
- `net status|udp|recv|listen|tcp`: inspect and exercise the IPv4/UDP/TCP
  loopback stack used by early network-debug work.

The shell is intentionally fixed-buffer and freestanding. Windowed QEMU mode
routes keyboard input into the shell after `OK_TEST_PASS`, then mirrors output
to serial and the display driver.
