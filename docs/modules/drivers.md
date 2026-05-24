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
  clear, pixel, rectangle, text-line, and checksum operations. Kernel boot writes
  Linux-style startup lines through this driver so `qemu-window-test` can show
  the same debug output that the QEMU checker validates. The GUI compositor also
  targets this logical framebuffer before platform code scales it to ramfb. The
  backend tag can be `memory_framebuffer`, `vga_text`, `ramfb`, or
  `virtio_gpu_pci`.
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

After boot creates the scheduler idle process, `DriverManager` also registers
most built-in drivers as background kernel processes named `drv:<driver-name>`.
RAM-only block manipulation stays an in-kernel helper and does not get a
`drv:ram-block0` process. The rest of the driver logic still runs in kernel
space, but `ps aux` can show the specific driver that owns each kernel process
slot when the shell is running as `kernel`.

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
  checksum.
- `ok::gui::GuiCompositor` is the restartable GUI module above the framebuffer.
  It owns fixed-capacity surfaces, rectangle/pixel/text drawing, and composition.
  It renders a short startup animation during boot and tracks normal,
  minimized, and maximized surface state. It also consumes platform mouse
  events for title-bar dragging, bottom-right resizing, and window control
  buttons. The debug shell renders command history and active input to a
  maximized `oksh` GUI surface while keeping serial output unchanged, and the
  `fm`/`fileman` command opens a separate GUI file-manager surface for VFS
  directory listings and mouse navigation.
- `RamFbConsole` is the QEMU-visible platform backend. It initializes `ramfb`
  through fw_cfg DMA, owns the 960x540 guest-RAM pixel surface, and scales
  logical GUI pixels through `ok_platform_display_gui_pixel()`.

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
remains as a fallback for headless serial use.
