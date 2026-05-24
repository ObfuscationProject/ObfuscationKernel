# Debug Shell

`ok::KernelDebugShell` is a kernel-owned command shell for debug builds and
windowed QEMU sessions. It does not provide a separate test executable or hosted
`main`; it is attached during the normal `kernel_main` boot path.

The interactive line editor keeps the prompt outside the editable buffer:
Backspace/Delete only erase user input, and Ctrl-U clears the current input
line. Ctrl-C clears the current line or interrupts the foreground GUI child
process. Up/Down recall command history in the same style as common Linux
shells.
In windowed GUI mode, mouse wheel input scrolls the shell's visual scrollback
with the same direction convention as Windows: wheel up moves toward older
scrollback, and wheel down returns toward the prompt.
F12 creates a fresh GUI shell window; while the active session is the `kernel`
user this is the kernel debug shell. In GUI mode each visible shell window is
registered as an `oksh` scheduler process.
The command evaluator follows the Bourne shell subset needed for kernel debug
work: comments beginning with `#`, command sequences separated by `;`, and
basic `&&`/`||` conditionals.

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
- `pwd`, `cd <path>`, `ls [-a] [-h] [-l] [path]`, `cat <path>`, `touch <path>`,
  `mkdir <path>`, `rm <path>`, `stat <path>`: BusyBox-style file commands over
  the RAM VFS/POSIX layer.
- `chmod <octal> <path>`, `chown <user> <path>`: manage VFS ownership and
  permissions through POSIX credentials.
- `whoami`, `id`, `su [user]`: switch the debug shell session through the kernel
  user manager; privileged sessions may become `kernel`, `root`, `user`, or any
  account registered by tests/modules.
- `users`: list debug-shell-visible users. The `kernel` account is scoped to the
  kernel debug shell.
- `kill <pid>`: forcefully remove a scheduler process and tear down owned
  kernel UI surfaces such as `fm:<user>` or `oksh`. Kernel-space processes can
  only be killed from the `kernel` debug-shell user; `idle` is protected.
  Killing supervised `drv:*` or `mod:*` daemon processes forces the kernel
  guards to recreate them and append a restart line to `/tmp/kernel.log`.
- `shutdown [now|-h|-h now|-r|-r now]`, `poweroff`, `halt`, `reboot`: request a
  system power action from the `kernel` debug-shell user. `shutdown` and
  `poweroff` request poweroff by default, `shutdown -r` requests reboot, and
  `halt` enters the platform halt path when no debug-exit device consumes the
  request.
- `exit`: leave the current debug shell user context, restoring the previous
  session user. With no previous user, non-kernel sessions return to `kernel`;
  exiting the base `kernel` session closes the GUI shell surface.
- `disk`: print active block-device geometry and SimpleFS mount state.
- `mkfs [label]`: format the active block device as SimpleFS.
- `sfs info|ls|touch|write|cat|stat|rm`: operate directly on the SimpleFS flat
  root directory for block-filesystem testing.
- `ext4 status|disk`: report EXT4 parser/block-mount state or try mounting the
  current block device as EXT4.
- `net status|udp|recv|listen|tcp`: inspect and exercise the IPv4/UDP/TCP
  loopback stack used by early network-debug work.
- `fm [path]` / `fileman [path]`: fork a foreground GUI kernel file manager for
  a VFS directory as a scheduler-visible `fm:<user>` process using the current
  credentials. While that foreground file manager is open, the debug shell
  process is blocked and does not display a fresh prompt until the file manager
  closes.
- `F1`: open or raise the GUI file manager at the current working directory
  without blocking the shell.

The shell is intentionally fixed-buffer and freestanding. Windowed input is
dispatched through GUI focus: keyboard events reach `oksh` only while a shell
surface is focused. Debug test builds close their shell/file-manager GUI
surfaces after `OK_TEST_PASS`, try the debug-exit path, and keep the desktop
event loop alive if the platform returns from that path. The taskbar launchers
can then reopen the shell or file manager without leaving stale serial debug
output attached to the GUI. The GUI title strip and terminal body are rendered
with separate colors. Showing the GUI shell or running `clear` resets scrollback
while immediately redrawing a fresh `ok> ` prompt. The legacy display-driver
text path is still used for boot logs, `OK_DISPLAY_TEXT` diagnostics, and
fallback when the GUI cannot be restarted.
