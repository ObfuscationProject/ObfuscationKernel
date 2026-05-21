# Boot Profiles

Bootable targets are split from the freestanding `okernel` library. The
`okernel_image` target links `okernel` with one platform entry path and produces
`kernel.bin`.

## Implemented

- `i386`: BIOS disk image, shared x86 boot sector, protected-mode entry, VGA,
  COM1 serial, PS/2 keyboard and mouse polling.
- `x86_64`: BIOS disk image, shared x86 boot sector, protected-mode to long-mode
  entry, identity page tables, VGA, COM1 serial, PS/2 keyboard and mouse
  polling.
- `aarch64`: Linux `Image`-style 64-byte header and QEMU virt PL011 UART
  platform path. `qemu-system-aarch64` boots the debug image directly with
  `-kernel` and validates the serial `OK_TEST_PASS` marker.
- `arm32`: QEMU virt direct ELF boot at `0x40008000`, bootstrap stack, `.bss`
  clearing, PL011 UART output, ramfb display, and virtio-mmio keyboard/mouse
  input.
- `rv64`: QEMU virt direct ELF boot at `0x80000000`, bootstrap stack, `.bss`
  clearing, NS16550 UART output, and `qemu-system-riscv64` debug validation.
- `rv32`: QEMU virt direct ELF boot at `0x80000000`, bootstrap stack, `.bss`
  clearing, NS16550 UART output, ramfb display, and virtio-mmio keyboard/mouse
  input.

## Protocol Notes

- ARM64 Linux images start with a 64-byte header containing executable branch
  words, `text_offset`, `image_size`, flags, and the `ARM\x64` magic. The boot
  environment must also prepare RAM and a device tree before calling the image.
- ARM32 Linux boot expects boot data through ATAGs or a device tree pointer in
  `r2`, with `r0 = 0` and `r1` carrying the machine type for non-DT systems.
  The current ARM32 QEMU test intentionally uses direct ELF loading instead of
  the Linux zImage protocol.
- RISC-V Linux images use a 64-byte header with text offset, image size,
  version, and `RISCV`/`RSC\x05` magic values; image size is mandatory. The
  current RV64 QEMU test uses the simpler `-bios none -kernel` ELF path so QEMU
  transfers control directly to the kernel entry at the DRAM base.
- BMIPS DT-aware boot passes `a0 = 0`, `a1 = 0xffffffff`, and `a2 = DTB physical
  pointer`.
- PowerPC Linux uses wrapper formats because there is no single firmware
  interface; image flavor depends on OpenFirmware, U-Boot, or board-specific
  conventions.
- LoongArch Linux images are EFI/PE-style images and enter with arguments for
  EFI boot state, command line, and system table.
