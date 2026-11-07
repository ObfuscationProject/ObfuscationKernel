# QEMU Testing

The QEMU tests boot the compiled `kernel.bin` directly. There is no test
`main.cpp`, hosted debug wrapper, GRUB image, or external bootloader. The image
contains a small kernel-owned first-stage boot sector plus the linked kernel
payload. Both debug and release builds enter the same `kernel_main`; debug mode
adds `OK_ENABLE_TEST_POINTS` and emits serial/display diagnostics for the Python
runner to validate.

## Current Architecture Test

Configure one architecture with xmake's built-in `-a` option:

```sh
xmake f -c -y -m debug -a x86_64
xmake test
```

`xmake qemu-test` is a convenience wrapper for the same current-architecture
test:

```sh
xmake qemu-test
```

Pass `-a` only for a temporary one-off test of another architecture:

```sh
xmake qemu-test -a i386
```

Use the matrix tasks when validating architecture coverage from one checkout:

```sh
xmake profile-matrix
xmake qemu-matrix
xmake qemu-window-matrix
```

`profile-matrix` compiles the freestanding `okernel` static profile for every
supported architecture. `qemu-matrix` and `qemu-window-matrix` build and boot
the debug `okernel_image` target for every supported architecture.

The current bootable QEMU system test targets are `i386`, `x86_64`, `aarch64`,
`arm32`, `rv64`, `rv32`, `loongarch64`, `mips`, `mips64`, and `ppc`.

`xmake test` always has a freestanding profile test for `okernel` and runs the
`okernel_image` QEMU boot test for the configured architecture.

Every booted QEMU check also creates a temporary 16 MiB disk image and attaches
it as `virtio-blk-pci`. The kernel binds that PCI device as `virtio-blk0`, then
formats and exercises SimpleFS/EXT4 through the generic block-device interface.
The debug suite also exercises UDP and TCP loopback through the kernel network
stack, and the Python runner emits `OK_QEMU_NET_TEST` when that coverage passes.

## Visual Test

The windowed task defaults to the current architecture:

```sh
xmake qemu-window-test
```

Pass `-a` to run another architecture without permanently switching the
checkout:

```sh
xmake qemu-window-test -a rv32 --no-launch
xmake qemu-window-test -a ppc --no-launch
```

For every supported architecture, it builds and runs the same `kernel.bin` in
QEMU.

The visible pixel output comes from the kernel's own ramfb display path on
architectures whose QEMU machine exposes fw_cfg: x86/i386 use fw_cfg I/O ports,
while `aarch64`, `arm32`, `rv64`, `rv32`, and `loongarch64` use fw_cfg MMIO.
`mips`, `mips64`, and `ppc` use QEMU Malta/ppce500 machines without
fw_cfg/standalone `ramfb`, so their window path falls back to a serial VC while
the kernel still exercises its generic memory framebuffer and GUI compositor
state.
The script captures serial diagnostics and prints the test result only after
the QEMU window is closed. Use the headless validation form in environments
without a graphical display:

```sh
xmake qemu-window-test --no-launch
```

In graphical debug-test mode the kernel runs the same validation suite, prints
`OK_TEST_PASS` to the serial console, closes debug GUI surfaces, and then tries
the debug-exit path. If the platform returns from debug-exit, the kernel stays
in the desktop event loop instead of halting, but it does not automatically
spawn a background `oksh`; the taskbar launchers and the F12/F1 shortcuts can
open shell or file manager windows again. Non-test graphical sessions route
keyboard input through the focused GUI surface: `oksh` receives text only while
focused, and the file manager consumes simple navigation keys.
Because this interactive path still polls QEMU input queues, desktop idle uses
the platform polling-idle hook rather than `hlt`/`wfi`/`wfe`; those real halt
instructions remain reserved for explicit halt/poweroff paths.
The ramfb backend scales the kernel's logical GUI framebuffer into the full
960x540 pixel surface. Shutdown commands are explicit debug-shell requests:
`shutdown`/`poweroff` request poweroff, `halt` enters the halt path, and
`reboot` requests the platform reboot path before falling back to debug-exit.

For non-debug graphical sessions, release mode keeps the GUI desktop behind the
independent `kernel_gui` build option so ordinary release images stay quiet by
default. Use the launcher task to build with that option and start a QEMU
window:

```sh
xmake qemu-gui
xmake qemu-gui -a aarch64 --display=sdl
```

For `aarch64`, `arm32`, `rv64`, and `rv32` window sessions, QEMU attaches
`virtio-keyboard-device` and `virtio-mouse-device`; the guest consumes their
legacy virtio-mmio event queues so keyboard input reaches the shell and mouse
motion updates the framebuffer pointer. x86/i386 use PS/2 keyboard and mouse
events from the PC platform while rendering through ramfb.

## Pass Signal

The debug kernel must print `OK_MODE debug`, `OK_DEBUG boot=complete`,
`OK_DISPLAY_TEXT`, and `OK_TEST_PASS`. The Python runner also verifies that
debug test points ran and that filesystem, EXT4, user-mode, and display checks
reported success. The current required coverage fields are `fs`, `simplefs`,
`ext4`, `user`, `display`, `gpu`, `input`, `posix`, `bus`, `usb`, `net`,
`shell`, `modes`, and `gui`. The bootable system targets advertise the same
QEMU capability set, so headless validation requires the same fields for all
supported architectures. Any non-zero exit code or missing marker is a failure.

## CI Coverage

GitHub Actions has one matrix job. For each architecture it:

1. Builds and resolves the matching `systoolchain` package through xmake; the
   package supplies both the freestanding compiler and QEMU.
2. Configures xmake with `-a <arch>`.
3. Builds `okernel`.
4. Runs `xmake test`, which immediately boots the generated kernel in QEMU for
   the configured architecture.

## Coverage scope

`OK_TEST_PASS` means the debug suite passed after boot; it is not a POSIX or
hardware-feature certification. `OK_VM`, `OK_PROC`, `OK_ELF`, `OK_USERLAND`,
`OK_SMP`, `OK_IRQ` and `OK_PREEMPT` identify `scope=model`. Their success covers
state transitions and metadata, not process MMU isolation, user ELF execution,
secondary-CPU startup, actual interrupt delivery or context switching.

`OK_MODULE_METADATA` replaces the misleading `OK_MODULE_LOAD` label. The parser
checks ELF/OKMOD metadata and relocation records without executing relocated
code. The Python summary preserves each marker's scope instead of promoting a
model result to hardware coverage.

The OS owns system-desktop integration tests. Run its `distro_image` with the
matching exported SDK to exercise extension attachment, package dispatch and
GUI lifecycle; a standalone kernel does not include the OS desktop or Linux
driver compatibility model.

## SMP lane

`scripts/qemu_test.py --smp N` (1 to 8) appends `-smp N` to every profile's
QEMU command line; the default of 1 leaves the command line unchanged. The
xmake option `qemu_smp` (default 1) forwards it from `xmake run`, `xmake test`,
`xmake qemu-test` and `xmake qemu-matrix`:

```sh
xmake f -c -y -m debug -a x86_64 --qemu_smp=2
xmake test
```

With N above 1 the runner requires the hardware marker
`OK_SMP scope=hardware cpus=<discovered> online=<online> ap_start=<method>
ipi=<pass|fail|unsupported>`, with a trailing `reason=<token>` when the kernel
stays uniprocessor. `x86_64`, `aarch64` and `rv64` must report `cpus=N`,
`online=N` and `ipi=pass`; every other profile must report `online=1` and
`ap_start=unsupported`. A missing or mismatched marker fails with exit code 14.
The kernel prints the hardware line before `OK_TEST_PASS`: every profile except
i386 and x86_64 stops capturing output at that marker. Under `-bios none` QEMU
starts every RISC-V hart at the kernel entry, so rv64 and rv32 park the harts
other than hart 0 before they touch the stack or BSS.
The `scope=model` `OK_SMP` line is not evidence of secondary-CPU startup and
never satisfies this check.

On the SMP profiles the runner also checks, with exit code 14 on failure:

- `tsc=` (x86_64 once a secondary is online): one of `sync`, `adjusted` or
  `offset`, equal to the worst of the `OK_TSC_SYNC` lines, of which there must
  be exactly one per secondary. Each must pass the kernel's own acceptance
  recomputed from its fields (error within `max_error`, a `sync` offset within
  error plus slack, a corrected residual within its error plus tolerance).
  Under QEMU every vCPU reads one counter, so each must be `sync`, except with
  `--tsc-skew-test` (forwarded by xmake when the debug option
  `x86_tsc_skew_test` is set), where CPU 1 must report `skew_test=tsc` or
  `software` and be detected and corrected.
- `OK_SMP_TLB scope=hardware asid=<bits|none> pcid=<on|off>`, required on
  x86_64, aarch64 and rv64 with an allowed mode: x86_64 `none`/`off` or
  `12`/`on`, aarch64 `8` or `16` with `off`, rv64 `none` or 1 to 16 bits with
  `off`.
- The CPU hotplug lines, when present (only the SMP profiles may print
  them): `OK_SMP_HOTPLUG_BOOT` at most once, naming `cpu=1` with `stop`,
  `parked_ipi`, `restart` and `ipi` all `pass` (every debug SMP boot prints
  it); and the `smp_hotplug_test` knob's repeated `OK_SMP_HOTPLUG` lines,
  whose counters never go back and whose CPU never changes, the last showing
  `cycles` above 0, `failures=0` and a secondary CPU.

`OK_QEMU_SMP_TEST` then appends `tsc=`, `tlb_asid=`, `pcid=`,
`hotplug_boot=pass` and `hotplug_cycles=` as they were checked. The summary
records `smp.cpus`, the distinct scopes of each marker under
`scopes.<marker>`, `marker.OK_SMP_HARDWARE`, the hardware marker's fields as
`smp.hardware.<field>`, the TLB line's as `smp.tlb.<field>` and the hotplug
lines' as `smp.hotplug_boot.<field>` and `smp.hotplug.<field>`.

`OK_SMP_CPUS`, `OK_SMP_IRQS` and the `OK_SMP_KERNEL` lines come from the
native runtime's interrupt report, which standalone lane transcripts do not
contain; the OS distro runner and the OS SMP gates check them
(`docs/VALIDATION.md`).
