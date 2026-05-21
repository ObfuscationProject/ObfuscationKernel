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
  the same debug output that the QEMU checker validates. The backend tag can be
  `memory_framebuffer`, `vga_text`, `ramfb`, or `virtio_gpu_pci`.
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

QEMU tests attach a temporary `virtio-blk-pci` disk on every bootable target.
QEMU window tests attach `ramfb` on every bootable architecture. The platform
code initializes ramfb through the QEMU fw_cfg DMA interface and draws into
guest RAM, so the visible window is a real pixel framebuffer rather than a
serial console or VGA text surface. The current driver model still validates
virtio-gpu enumeration through the internal PCI model; BAR mapping, virtqueues,
EDID, scanout resource management, and true disk DMA are the next implementation
steps.

On `aarch64` and `rv64`, graphical keyboard and mouse input is delivered through
QEMU virtio-mmio input devices. The platform code sets up the virtio event queue,
maps Linux input key codes into shell characters, and moves a small framebuffer
pointer for mouse-relative events. UART input remains as a fallback for headless
serial use.
