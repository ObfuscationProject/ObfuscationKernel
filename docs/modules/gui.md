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

The compositor API is intentionally small and synchronous. The base compositor
types live in `include/ok/gui/compositor.hpp`; `include/ok/gui/gui.hpp` remains
as a compatibility aggregate for callers that still want all GUI declarations.

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
  shell window is focused, while kernel applications such as the file manager
  and task/top consume their own navigation keys when focused.
- GUI scroll input uses a single Windows-like convention: positive rows mean
  wheel-up/previous content, and negative rows mean wheel-down/next content.
  Shell scrollback and task/top process lists translate that shared direction
  through their own offset models.
- debug-test cleanup closes shell, file-manager, and task-monitor surfaces.
  If a graphical platform stays open after `OK_TEST_PASS`, the desktop starts
  with no background `oksh`; the taskbar launchers and F12/F1 shortcuts can open
  new windows on demand. Headless x86/i386 tests still exit through
  `isa-debug-exit`, and headless non-x86 runners stop QEMU once they see
  `OK_TEST_PASS`.
- the desktop event loop is polling-driven while keyboard and mouse IRQ wakeups
  are still simulated, so its idle path uses a lightweight platform relax hook
  instead of `hlt`/`wfi`/`wfe`.
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

The compositor renders a complete logical frame and submits it to
`FramebufferDisplayDriver`, whose logical mode is currently 480x270 RGBA pixels.
The compositor does not call QEMU or platform hooks directly: the display driver
owns frame begin/frame/frame end hooks and falls back to per-pixel presentation
only when a platform has no frame presenter. QEMU ramfb platforms scale the
logical frame onto the visible 960x540 framebuffer in their backend code. Each
present starts from a compositor-owned desktop background with a large geometric
`OK` artwork in kernel mode, so legacy boot/debug text is not left behind
outside GUI surfaces.

This keeps the module independent from QEMU-specific ramfb details:

- `FramebufferDisplayDriver` remains the portable kernel display device.
- `RamFbConsole` owns fw_cfg DMA setup and physical guest-RAM pixels.
- `GuiCompositor` owns surface state and composition.

The GUI module advertises per-CPU render worker threads when the display driver
uses CPU-side GUI frame composition, including ramfb. The process still presents
through `FramebufferDisplayDriver`, so changing the physical backend does not
require per-application rendering code.

RAMFB also owns the physical mouse pointer overlay. The display frame boundary
restores the previous pointer before a GUI frame is copied, then redraws it once
after the frame lands. This keeps realtime surfaces such as `top` from flickering
or erasing the pointer during automatic refreshes.

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
does not change the session restored by another `oksh` window. When a shell or
file manager is opened from a kernel session, its scheduler record stays a
kernel thread; when it is opened from `root` or another normal user, it is
created as an isolated user process with its own address-space ID.

The `fm`/`fileman` shell command runs the kernel file manager for a path. The
file-manager application interface lives under `ok::apps`, outside the
compositor API and outside `ModuleManager`, and renders the VFS directory listing in a separate
`kernel-files` surface using the same compositor. Mouse clicks in its left
navigation open `/`, `/dev`, `/tmp`, or `/proc` when present; clicks in the
listing select files, open directories, and show details/previews for regular
files, with `../` as the first entry outside `/` for parent directory
navigation. The window itself uses the same drag, resize, minimize,
maximize, and close handling as other GUI surfaces. Each open runs as a separate
`fm:<user>` scheduler process with the credentials active at launch, and
directory reads are checked against those credentials. A shell-launched file
manager blocks its launching `oksh` process until it exits, while F1 opens
another file manager as a shortcut without blocking the shell.

`taskman` and `top` live under `ok::apps` as kernel applications. `taskman tui`
renders a one-shot task-manager snapshot, `taskman gui` opens a foreground
`tm:<user>` GUI child, `top tui` renders a top-style snapshot, and `top gui`
opens a realtime foreground `top:<user>` GUI child. Automatic GUI refresh is
throttled by the kernel tick so the monitor remains live without repainting on
every event-loop spin. Ctrl-C interrupts a shell-launched GUI `top` like other
foreground programs. `taskman close` or `top close` closes the active monitor
GUI process and surface.
All views read the same scheduler, network, and block device counters, showing
per-CPU dispatch usage, current PID/TID, process CPU share, network byte
counters, and disk I/O bytes. Mouse wheel input scrolls the active task-manager
process list with the same GUI-wide wheel direction as shell scrollback.

The serial console and legacy framebuffer text path are preserved for boot logs,
automated QEMU validation, and GUI startup failure fallback. Once the GUI shell
surface is available, interactive framebuffer output is owned by the GUI. The
compositor writes logical pixels to `FramebufferDisplayDriver`; platform-visible
pixel submission is delegated back through the display driver instead of calling
platform hooks from the compositor directly.

## Tests

The debug and roadmap tests cover:

- GUI module startup and `gui.compositor` service publication;
- surface creation, metadata updates, z ordering, hit testing, drawing, and
  framebuffer checksum updates;
- compositor crash rejection and supervisor restart;
- `kernel-gui` ownership by the `mod:kernel-gui` kernel background process;
- mouse-driven window drag, resize, close, and GUI file-manager navigation;
- a post-suite input smoke test that clicks the taskbar shell launcher and uses
  the file-manager shortcut after the full debug test run;
- file-manager process ownership for `kernel` and non-kernel users;
- task-manager TUI/GUI rendering for CPU, network, disk, and process usage;
- startup animation and GUI file-manager rendering;
- shell command output rendering to a GUI surface and `oksh` process
  registration.

Successful debug runs emit:

```text
OK_GUI compositor=pass surface=pass restart=pass
OK_TEST_PASS ... gui=1 ...
```
