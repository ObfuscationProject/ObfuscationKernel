# Debug Shell

`ok::KernelDebugShell` is a kernel-owned command shell for debug builds and
windowed QEMU sessions. It does not provide a separate test executable or hosted
`main`; it is attached during the normal `kernel_main` boot path.

The interactive line editor keeps the prompt outside the editable buffer:
Backspace/Delete only erase user input, and Ctrl-U clears the current input
line. Platform keyboard code maps Ctrl-C to ETX (`0x03`), and the kernel routes
that key to the shell foreground process before normal focused-surface dispatch,
so a shell-launched GUI app can be interrupted even while its own window is
focused. Up/Down recall command history in the same style as common Linux
shells.
In windowed GUI mode, mouse wheel input is routed to the active surface. Shell
surfaces use the GUI-wide scroll convention: wheel up moves toward older
scrollback, and wheel down returns toward the prompt.
F12 creates a fresh GUI shell window. While the active session is the `kernel`
user this is a kernel-thread `oksh`; from `root` or another normal user it is a
normal user process with its own address-space ID. In GUI mode each visible
shell window is registered as a separate `oksh` scheduler process.
The command evaluator follows the Bourne shell subset needed for kernel debug
work: comments beginning with `#`, command sequences separated by `;`, and
basic `&&`/`||` conditionals. It also handles quotes, backslash escaping,
`$PWD`/`$USER`/`$?` and exported shell variables, `|` pipelines for built-in
filters, `<` input redirection, and `>`/`>>` output redirection.

Supported commands:

- `help`, `true`, `false`, `:`, `clear`, `uname`: basic shell commands.
- `status`: print architecture, process, CPU, driver, syscall, and POSIX state.
- `mem`: print frame allocator counters.
- `ps`: print scheduler/process state.
- `drivers`: print registered driver count and input-device status.
- `fs`: print RAM VFS state for `/tmp/kernel.log`.
- `posix`: print POSIX process, cwd, and open-file state.
- `test`: print the last debug-test coverage result.
- `echo [-n] <text>`: echo input through the kernel output path.
- `pwd`, `cd <path>`, `ls [-a] [-h] [-l] [path]`, `cat <path>`, `touch <path>`,
  `cp <src> <dst>`, `mv <src> <dst>`, `mkdir <path>`, `rm [-r|-d] <path>`,
  `rmdir <path>`, `stat <path>`: BusyBox-style file commands over the RAM
  VFS/POSIX layer. Long `ls` output includes owner and group names, so `ls -lha`
  shows entries in the same `mode links user group size name` shape.
- `grep`, `wc`, `head`, `tail`: built-in pipeline/input-redirection filters.
- `history`, `env`, `export`, `unset`, `type`, `which`: common shell inspection
  and environment commands.
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
- `fm [gui|tui|close] [path]` / `fileman [gui|tui|close] [path]`: run the
  kernel file manager. GUI mode forks a foreground scheduler-visible
  `fm:<user>` process using current credentials; TUI mode prints a directory
  table in the shell; `close` closes the active GUI file manager. These are
  kernel applications in `ok::apps`, not kernel modules.
- `taskman [tui|gui|close]`: print or open the task-manager application.
- `top [gui|tui|close]`: run the kernel `top` application. Plain `top` keeps the
  historical foreground GUI behavior; each GUI launch creates an independent
  `top:<user>` monitor window, while `top tui` prints a top-style snapshot in
  the shell.
- `F1`: open or raise the GUI file manager at the current working directory
  without blocking the shell.

The shell is intentionally fixed-buffer and freestanding. Windowed input is
dispatched through GUI focus: keyboard events reach `oksh` only while a shell
surface is focused. A shell with a foreground GUI child stays runnable for
terminal-style I/O and status reporting, but new shell commands wait until the
foreground child exits. Closing a shell window reaps its foreground GUI child
first so `top` or `fm` cannot outlive the shell that launched it. Debug test builds
close their shell/file-manager GUI surfaces after `OK_TEST_PASS`, try the
debug-exit path, assert that no orphan `oksh` process remains, and leave the post-test
desktop without a background `oksh` unless the user opens one. The GUI title
strip and terminal body are rendered with separate colors. Showing the GUI shell
or running `clear` resets scrollback while immediately redrawing a fresh `ok> `
prompt. The legacy display-driver text path is still used for boot logs,
`OK_DISPLAY_TEXT` diagnostics, and fallback when the GUI cannot be restarted.
