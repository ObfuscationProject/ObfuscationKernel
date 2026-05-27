# Drivers

Drivers derive from `ok::driver::Driver` and are managed by `DriverManager`.
Driver registration is constrained by the `KernelDriver` concept.

Built-in drivers:

- `ConsoleDriver`: appends output to an in-memory console buffer.
- `TimerDriver`: counts timer ticks.
- `NullBlockDriver`: accepts writes and returns zero-filled reads.
- `RamBlockDriver`: exposes a writable fixed-size RAM disk through the generic
  `BlockDevice` sector API for SimpleFS and future block-stack tests.
- `VirtioBlockDriver`: binds an emulated virtio block PCI device and exposes it
  through the same `BlockDevice` sector API. QEMU tests attach a temporary
  virtio-blk disk image so filesystem tests run against the virtual-disk path.
- `FramebufferDisplayDriver`: exposes a simple 32-bit RGBA framebuffer with
  clear, pixel, rectangle, GUI-frame present, GUI-pixel fallback, text-line, and
  checksum operations. Kernel boot writes
  Linux-style startup lines through this driver so `qemu-window-test` can show
  the same debug output that the QEMU checker validates. The GUI compositor also
  targets this logical framebuffer before the driver delegates frame presentation
  to platform code such as ramfb. The backend tag can be `memory_framebuffer`,
  `vga_text`, `ramfb`, or `virtio_gpu_pci`, and the driver reports whether GUI
  frame composition should use CPU-side render worker threads.
- `VirtioGpuPciDisplayDriver`: binds the emulated PCI display device and
  presents the kernel framebuffer through the virtio-gpu-pci path used by QEMU
  window tests.
- `KeyboardDriver`: decodes PS/2 set-1 scancodes into key events and ASCII
  characters in polling or interrupt mode.
- `Ps2MouseDriver`: queues PS/2 mouse packets for pointer/button input.
- `PciBusDriver`: records PCIe configuration-space style device identities and
  exposes enumeration to the kernel. The initial model publishes an emulated
  xHCI controller so bus/device probing is testable before real MMIO access is
  added.
- `UsbXhciControllerDriver`: owns the USB controller layer and exposes attached
  USB device descriptors.
- `UsbHidKeyboardDriver`: translates USB HID keyboard reports into kernel key
  events and ASCII input.
- `UsbHidMouseDriver`: translates USB HID mouse reports into pointer packets.

Driver I/O mode is represented by `IoMode` with polling, interrupt, and DMA
variants. The current x86 boot image uses polling for the early keyboard path;
the mode field is already part of `KernelConfig` so interrupt-backed drivers can
be enabled without changing the generic kernel entry.

After boot creates the scheduler idle process, `DriverManager` registers every
built-in driver as a background kernel daemon process named
`drv:<driver-name>`. The driver logic still runs in kernel space, but `ps aux`
can show the specific driver daemon that owns each scheduler process slot when
the shell is running as `kernel`.

Block storage uses `BlockDevice`, which reports geometry and performs aligned
sector reads/writes. `NullBlockDriver`, `RamBlockDriver`, and `VirtioBlockDriver`
implement that interface today; ATA, NVMe, USB mass storage, and full
virtio-blk virtqueue transport can share it later.

The PCIe/USB driver plan keeps discovery separate from transport:

- PCIe root/bus drivers own enumeration, BAR metadata, class/subclass/prog-if
  matching, and interrupt routing policy.
- Device drivers bind by stable IDs and class codes through `DriverManager`.
- USB host controller drivers expose controller-independent device, endpoint,
  keyboard, and mouse report objects.
- Real xHCI MMIO rings, MSI/MSI-X, DMA mapping, and hotplug handling are future
  transport implementations behind the existing driver interfaces.

## Display And GUI

The display stack has three layers:

- `FramebufferDisplayDriver` is the portable logical display device used by the
  kernel and tests. It stores a 480x270 RGBA framebuffer, boot text, and a stable
  checksum. GUI presents enter through a frame-level driver API, so the
  compositor stays independent from the physical display backend. Backends such
  as ramfb ask the GUI module for per-CPU render workers while still receiving
  one logical frame through this driver API.
- `ok::gui::GuiCompositor` is the restartable GUI module above the framebuffer.
  It owns fixed-capacity surfaces, rectangle/pixel/text drawing, and composition.
  It renders a short startup animation during boot and tracks normal,
  minimized, maximized, and focused surface state. Minimized surfaces are
  exposed through the bottom taskbar, while maximized surfaces leave the taskbar
  visible. Fixed taskbar launchers open another `oksh` or another `fm`. It also
  consumes platform mouse events for title-bar dragging,
  bottom-right resizing, and window control buttons. Keyboard input is routed to
  the focused surface.
  The debug shell renders command history and active input to a
  resizable `oksh` GUI surface while keeping serial output unchanged, and the
  `fm`/`fileman` command opens a separate GUI file-manager surface for VFS
  directory listings and mouse navigation.
- `RamFbConsole` is the QEMU-visible platform backend. It initializes `ramfb`
  through fw_cfg DMA, owns the 960x540 guest-RAM pixel surface, scales logical
  GUI frames through the display driver's platform present hook, and redraws the
  hardware-facing pointer once at the end of each GUI frame.

QEMU tests attach a temporary `virtio-blk-pci` disk on every bootable target.
QEMU window tests attach `ramfb` on bootable architectures whose QEMU machine
exposes fw_cfg. The visible window is therefore a real pixel framebuffer rather
than a serial console or VGA text surface on `i386`, `x86_64`, `aarch64`,
`arm32`, `rv64`, `rv32`, and `loongarch64`. QEMU Malta and ppce500 do not expose
fw_cfg/standalone `ramfb`, so `mips`, `mips64`, and `ppc` use the serial VC
window fallback. The ramfb console uses spaced bitmap glyph rendering for legacy
boot/debug output, then lets GUI presents take over the full visible framebuffer.

The current driver model still validates virtio-gpu enumeration through the
internal PCI model; BAR mapping, virtqueues, EDID, scanout resource management,
and true disk DMA are the next implementation steps.

On `aarch64`, `arm32`, `rv64`, and `rv32`, graphical keyboard and mouse input is
delivered through QEMU virtio-mmio input devices. The platform code supports the
legacy virtio-mmio queue registers used by QEMU `virt` here, maps Linux input
key codes into shell characters, forwards scaled logical mouse-relative events
into the GUI compositor, and then moves a small framebuffer pointer. UART input
remains as a fallback for headless serial use. F12 creates a fresh GUI shell
window, F1 opens a fresh GUI file manager, and Ctrl-letter combinations map to
the conventional control characters used by the shell, including Ctrl-C.
