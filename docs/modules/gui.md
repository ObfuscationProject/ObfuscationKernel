# GUI

The GUI layer lives above the framebuffer driver and below user-facing shells or
future window managers. It is intentionally small and fixed-capacity so it can
run in the freestanding kernel profile without heap allocation.

## Module

`ok::gui::GuiModule` is a kernel module named `kernel-gui`. During boot the
kernel binds it to `FramebufferDisplayDriver`, starts it through
`ModuleManager`, and publishes the `gui.compositor` service. The manifest uses
`ModuleExecution::kernel_process`, so the module is managed by the scheduler
visible `mod:kernel-gui` background process rather than the core boot path.

The module owns `GuiCompositor`, which provides:

- fixed-capacity surfaces with bounds, title, visibility, and z order metadata;
- fill, rectangle fill, pixel write, fixed-cell text drawing, title update,
  move, resize, visibility, raise, and hit-test operations;
- composition into the logical kernel framebuffer;
- desktop bounds and surface metadata queries;
- a generation counter and last-present checksum for tests;
- a crash state used to reject new work until a supervisor restarts the module.

`GuiSupervisor` watches the compositor state. If the compositor is marked
crashed, the supervisor calls `ModuleManager::restart_module("kernel-gui")`.
Restarting clears volatile surfaces, increments the generation counter, keeps
the same service registration, reuses the module process record, and leaves the
rest of the kernel running.

During boot, `GuiCompositor::play_startup_animation()` presents a short
multi-frame startup animation before the shell claims the desktop.

## API Contract

The compositor API is intentionally small and synchronous:

- `create_surface(bounds, title)` allocates one of the fixed backing stores.
- `set_title`, `move_surface`, `resize_surface`, `set_visible`, and
  `raise_surface` update surface metadata.
- `minimize_surface`, `maximize_surface`, `restore_surface`, and
  `close_surface` provide Windows-style window state controls. Minimized
  surfaces dock into a persistent bottom taskbar so a mouse click can restore
  them. The taskbar also has fixed debug-shell and file-manager launchers; the
  debug-shell launcher opens another isolated `oksh` window, while the
  file-manager launcher opens another file-manager window.
  Maximized surfaces use the desktop work area above the taskbar.
- `handle_mouse_delta()` tracks the logical pointer and turns button presses into
  title-bar drag, bottom-right resize, minimize, maximize/restore, close-request
  events, raise, focus, and hit-test behavior. The minimize control restores a
  maximized window to windowed mode before it minimizes a normal window.
  Kernel-owned surfaces route close requests through their owning process before
  any forced close.
- keyboard input is routed by the focused surface: `oksh` receives text when a
  shell window is focused, while the file manager consumes simple navigation keys
  when it is focused.
- debug-test cleanup closes shell and file-manager surfaces, then leaves the
  desktop taskbar responsive if the platform debug-exit path returns. This keeps
  post-test input usable until an explicit `shutdown`, `poweroff`, `halt`, or
  `reboot` command is requested.
- `fill`, `fill_rect`, `put_pixel`, and `draw_text` update backing pixels.
- `surface_at(x, y)` returns the top visible surface at a logical desktop
  coordinate.
- `desktop_bounds()` returns the logical framebuffer rectangle.
- `play_startup_animation()` renders the GUI boot animation and records frame
  count for tests.
- `present()` composes each final framebuffer pixel from the desktop and the
  topmost visible surface at that point, avoiding transient desktop clears under
  windows during mouse-driven redraws.

All calls return `Status`/`Result<T>` and use fixed-capacity storage. Resizing a
surface preserves existing pixels inside the old bounds and initializes newly
visible pixels with the default surface color.

## Display Path

The compositor renders into `FramebufferDisplayDriver`, whose logical mode is
currently 480x270 RGBA pixels. `present()` also calls the weak platform hook
`ok_platform_display_gui_pixel()` when a platform provides it. QEMU ramfb
platforms use that hook to scale logical GUI pixels onto the visible 960x540
framebuffer. Each present starts from a compositor-owned desktop background with
a large geometric `OK` artwork in kernel mode, so legacy boot/debug text is not
left behind outside GUI surfaces.

This keeps the module independent from QEMU-specific ramfb details:

- `FramebufferDisplayDriver` remains the portable kernel display device.
- `RamFbConsole` owns fw_cfg DMA setup and physical guest-RAM pixels.
- `GuiCompositor` owns surface state and composition.

## Shell

`KernelDebugShell::execute()` still returns the same command output used by
serial tests and callers. After each command it also updates an `oksh` GUI
surface:

- the command is appended with an `ok> ` prompt;
- command output is appended to a bounded history buffer;
- `clear` output resets the GUI history and redraws a clean `ok> ` prompt;
- the active input line is redrawn by the GUI while the serial console still
  receives text output;
- the title strip and body use separate colors so the shell window has a
  distinct header;
- the surface is redrawn with `GuiCompositor::draw_text()` and presented.
Each GUI shell window tracks its own debug user, previous `su` context, input
line, scrollback, and foreground child process. Switching users in one `oksh`
does not change the session restored by another `oksh` window.

The `fm`/`fileman` shell command forks a foreground GUI kernel file manager for
a path. It renders the VFS directory listing in a separate `kernel-files`
surface using the same compositor. Mouse clicks in its left navigation open
`/`, `/dev`, `/tmp`, or `/proc` when present; clicks in the listing select
files and open directories, with `../` as the first entry outside `/` for parent
directory navigation. The window itself uses the same drag, resize, minimize,
maximize, and close handling as other GUI surfaces. Each open runs as an
`fm:<user>` scheduler process with the credentials active at launch, and
directory reads are checked against those credentials. A shell-launched file
manager blocks its launching `oksh` process until it exits, while F1 opens
another file manager as a shortcut without blocking the shell.

The serial console and legacy framebuffer text path are preserved for boot logs,
automated QEMU validation, and GUI startup failure fallback. Once the GUI shell
surface is available, interactive framebuffer output is owned by the GUI.

## Tests

The debug and roadmap tests cover:

- GUI module startup and `gui.compositor` service publication;
- surface creation, metadata updates, z ordering, hit testing, drawing, and
  framebuffer checksum updates;
- compositor crash rejection and supervisor restart;
- `kernel-gui` ownership by the `mod:kernel-gui` kernel background process;
- mouse-driven window drag, resize, close, and GUI file-manager navigation;
- file-manager process ownership for `kernel` and non-kernel users;
- startup animation and GUI file-manager rendering;
- shell command output rendering to a GUI surface and `oksh` process
  registration.

Successful debug runs emit:

```text
OK_GUI compositor=pass surface=pass restart=pass
OK_TEST_PASS ... gui=1 ...
```
