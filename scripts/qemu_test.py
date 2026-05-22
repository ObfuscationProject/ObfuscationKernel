#!/usr/bin/env python3
"""Boot an ObfuscationKernel kernel.bin in QEMU and validate debug output."""

from __future__ import annotations

import argparse
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


QEMU_SYSTEM_BY_ARCH = {
    "i386": "qemu-system-i386",
    "x86_64": "qemu-system-x86_64",
    "aarch64": "qemu-system-aarch64",
    "arm32": "qemu-system-arm",
    "rv64": "qemu-system-riscv64",
    "rv32": "qemu-system-riscv32",
    "loongarch64": "qemu-system-loongarch64",
    "mips": "qemu-system-mips",
    "mips64": "qemu-system-mips64",
    "ppc": "qemu-system-ppc",
}

QEMU_DEBUG_EXIT_SUCCESS = 33
VIRTUAL_DISK_SIZE = 16 * 1024 * 1024
BASE_COVERAGE_FIELDS = ("fs", "simplefs", "ext4", "user", "posix", "shell", "modes", "gui")
CAPABILITY_COVERAGE_FIELDS = {
    "display": ("framebuffer", "ramfb", "virtio_gpu"),
    "gpu": ("virtio_gpu",),
    "input": ("keyboard_input", "mouse_input"),
    "bus": ("pci_bus",),
    "usb": ("usb_hid",),
    "net": ("network_loopback",),
}
ALL_COVERAGE_FIELDS = BASE_COVERAGE_FIELDS + tuple(CAPABILITY_COVERAGE_FIELDS)
BOOTABLE_QEMU_ARCHES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")
BOOTABLE_QEMU_CAPABILITIES = (
    "serial_console", "framebuffer", "keyboard_input", "mouse_input", "pci_bus",
    "virtio_block", "virtio_gpu", "ramfb", "usb_hid", "network_loopback",
)

CAPABILITIES_BY_ARCH = {arch: BOOTABLE_QEMU_CAPABILITIES for arch in BOOTABLE_QEMU_ARCHES}


class QemuConfigurationError(RuntimeError):
    """Raised before QEMU starts when the requested launch profile is invalid."""


def normalize_arch(arch: str) -> str:
    aliases = {
        "x64": "x86_64",
        "arm": "arm32",
        "arm64": "aarch64",
        "riscv64": "rv64",
        "riscv32": "rv32",
        "loong64": "loongarch64",
        "loongarch": "loongarch64",
        "powerpc": "ppc",
    }
    return aliases.get(arch, arch)


def arch_capabilities(arch: str) -> set[str]:
    return set(CAPABILITIES_BY_ARCH.get(arch, ()))


def required_coverage_fields(arch: str) -> set[str]:
    capabilities = arch_capabilities(arch)
    fields = set(BASE_COVERAGE_FIELDS)
    for field, required_caps in CAPABILITY_COVERAGE_FIELDS.items():
        if any(capability in capabilities for capability in required_caps):
            fields.add(field)
    return fields


def coverage_statuses(arch: str, fields: dict[str, str]) -> dict[str, str]:
    required = required_coverage_fields(arch)
    statuses: dict[str, str] = {}
    for field in ALL_COVERAGE_FIELDS:
        if field in required:
            if field not in fields:
                statuses[field] = "missing"
            elif fields[field] == "1":
                statuses[field] = "pass"
            else:
                statuses[field] = "fail"
            continue
        statuses[field] = "pass" if fields.get(field) == "1" else "skip"
    return statuses


def create_virtual_disk(path: Path) -> None:
    with path.open("wb") as handle:
        handle.truncate(VIRTUAL_DISK_SIZE)


def virtio_disk_args(disk: Path) -> list[str]:
    return [
        "-drive",
        f"file={disk},format=raw,if=none,id=fsdisk",
        "-device",
        "virtio-blk-pci,drive=fsdisk",
    ]


def ramfb_args() -> list[str]:
    return ["-device", "ramfb"]


def virtio_mmio_input_args() -> list[str]:
    return ["-device", "virtio-keyboard-device", "-device", "virtio-mouse-device"]


def virtio_pci_input_args() -> list[str]:
    return ["-device", "virtio-keyboard-pci", "-device", "virtio-mouse-pci"]


def serial_args(arch: str, display: str) -> list[str]:
    if arch in ("mips", "mips64", "ppc") and display != "none":
        return ["-serial", "vc:80Cx24C", "-serial", "stdio"]
    return ["-serial", "stdio"]


def qemu_command(arch: str, kernel: Path, display: str, debug_exit: bool, disk: Path) -> list[str]:
    qemu = QEMU_SYSTEM_BY_ARCH.get(arch)
    if qemu is None:
        raise QemuConfigurationError(f"qemu-system boot is not implemented for {arch} yet")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise QemuConfigurationError(f"QEMU executable missing: {qemu} was not found in PATH")

    if arch in ("aarch64", "arm32"):
        command = [
            qemu_path,
            "-M",
            "virt",
            "-cpu",
            "cortex-a57" if arch == "aarch64" else "cortex-a15",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_mmio_input_args()
        command += virtio_disk_args(disk)
        return command
    if arch in ("rv64", "rv32"):
        command = [
            qemu_path,
            "-M",
            "virt",
            "-m",
            "256M",
            "-bios",
            "none",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_mmio_input_args()
        command += virtio_disk_args(disk)
        return command
    if arch == "loongarch64":
        command = [
            qemu_path,
            "-M",
            "virt",
            "-m",
            "2G",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_pci_input_args()
        command += virtio_disk_args(disk)
        return command
    if arch in ("mips", "mips64"):
        command = [
            qemu_path,
            "-M",
            "malta",
        ]
        if arch == "mips64":
            command += ["-cpu", "MIPS64R2-generic"]
        command += [
            "-vga",
            "none",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += serial_args(arch, display)
        command += virtio_disk_args(disk)
        return command
    if arch == "ppc":
        command = [
            qemu_path,
            "-M",
            "ppce500",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += serial_args(arch, display)
        command += virtio_disk_args(disk)
        return command

    command = [
        qemu_path,
        "-drive",
        f"file={kernel},format=raw,if=ide",
        "-boot",
        "c",
        "-serial",
        "stdio",
        "-monitor",
        "none",
        "-no-reboot",
        "-vga",
        "none",
        "-display",
        display,
    ]
    command += ramfb_args()
    command += virtio_disk_args(disk)
    if debug_exit:
        command += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    return command


def run_kernel(arch: str, kernel: Path, display: str, debug_exit: bool, timeout: float | None) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="okernel-qemu-") as tmp:
        runnable_kernel = Path(tmp) / "kernel.bin"
        scratch_disk = Path(tmp) / "fs.img"
        shutil.copyfile(kernel, runnable_kernel)
        create_virtual_disk(scratch_disk)
        command = qemu_command(arch, runnable_kernel, display, debug_exit, scratch_disk)
        if not debug_exit:
            return run_until_marker(command, timeout)
        return subprocess.run(command, text=True, capture_output=True, check=False, timeout=timeout)


def run_until_marker(command: list[str], timeout: float | None) -> subprocess.CompletedProcess[str]:
    process = subprocess.Popen(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout_parts: list[str] = []
    deadline = None if timeout is None else time.monotonic() + timeout
    marker_seen = False

    assert process.stdout is not None
    while True:
        if deadline is not None and time.monotonic() >= deadline:
            break
        wait = 0.1 if deadline is None else max(0.0, min(0.1, deadline - time.monotonic()))
        ready, _, _ = select.select([process.stdout], [], [], wait)
        if ready:
            line = process.stdout.readline()
            if line:
                stdout_parts.append(line)
                if line.startswith("OK_TEST_PASS "):
                    marker_seen = True
                    break
        if process.poll() is not None:
            break

    if marker_seen and process.poll() is None:
        process.terminate()
    try:
        tail_stdout, stderr = process.communicate(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        tail_stdout, stderr = process.communicate()
    stdout = "".join(stdout_parts) + (tail_stdout or "")
    returncode = 0 if marker_seen else (process.returncode if process.returncode is not None else 124)
    if marker_seen:
        stderr = ""
    if not marker_seen and returncode == 0 and "OK_TEST_PASS " not in stdout:
        returncode = 124
        stderr = (stderr or "") + "qemu marker timeout\n"
    return subprocess.CompletedProcess(command, returncode, stdout, stderr or "")


def validate_output(arch: str, output: str, returncode: int, accept_debug_exit: bool) -> int:
    if accept_debug_exit and returncode != QEMU_DEBUG_EXIT_SUCCESS:
        if returncode == 124:
            print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
            return 12
        print(f"qemu did not exit through debug-exit success path: returncode={returncode}", file=sys.stderr)
        return returncode if returncode != 0 else 11
    if not accept_debug_exit and returncode != 0:
        if returncode == 124:
            print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
            return 12
        print(f"qemu exited with returncode={returncode}", file=sys.stderr)
        return returncode

    lines = output.splitlines()
    if "OK_MODE debug" not in lines:
        print("kernel did not boot in debug mode", file=sys.stderr)
        return 3
    if not any(line.startswith("OK_DEBUG boot=complete") for line in lines):
        print("debug kernel did not report boot completion", file=sys.stderr)
        return 4

    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    if not pass_lines:
        print("debug kernel did not report OK_TEST_PASS", file=sys.stderr)
        return 5

    fields = parse_fields(pass_lines[-1])
    if fields.get("arch") != arch:
        print(f"debug kernel arch mismatch: expected {arch}, got {fields.get('arch')}", file=sys.stderr)
        return 6
    if int(fields.get("debug_test_points", "0")) == 0:
        print("debug kernel did not run debug test points", file=sys.stderr)
        return 7

    coverage = coverage_statuses(arch, fields)
    required = required_coverage_fields(arch)
    for field in ALL_COVERAGE_FIELDS:
        if field not in required:
            if coverage[field] == "skip":
                print(f"OK_QEMU_SKIP arch={arch} coverage={field} reason=missing_capability")
            continue
        if coverage[field] == "missing":
            print(f"debug kernel did not report required coverage field: {field}", file=sys.stderr)
            return 8
        if coverage[field] == "fail":
            print(f"debug kernel did not pass required coverage field: {field}", file=sys.stderr)
            return 8

    if fields.get("display") == "1" and int(fields.get("display_checksum", "0")) == 0:
        print("display driver did not produce a framebuffer checksum", file=sys.stderr)
        return 9
    if fields.get("display") == "1" and not any(line.startswith("OK_DISPLAY_TEXT ") for line in lines):
        print("display driver did not report boot text", file=sys.stderr)
        return 10
    print(f"OK_QEMU_NET_TEST arch={arch} udp=pass tcp=pass")
    return 0


def write_summary(kernel: Path, arch: str, output: str, qemu_returncode: int, validation_code: int) -> None:
    summary = kernel.parent / "qemu-test-summary.txt"
    lines = output.splitlines()
    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    fields = parse_fields(pass_lines[-1]) if pass_lines else {}
    roadmap_markers = [
        "OK_MODULES",
        "OK_VM",
        "OK_PROC",
        "OK_ELF",
        "OK_USERLAND",
        "OK_VFS",
        "OK_DEVFS",
        "OK_PIPE",
        "OK_TTY",
        "OK_LINUX_ABI",
        "OK_LINUX_SYSCALLS",
        "OK_DRIVER_ABI",
        "OK_LINUX_DRIVER_SHIM",
        "OK_GUI",
        "OK_MODULE_LOAD",
        "OK_NETDEV",
        "OK_SOCK",
        "OK_BLOCK",
        "OK_EXT4_READONLY",
        "OK_SMP",
        "OK_IRQ",
        "OK_PREEMPT",
    ]
    marker_status = {
        marker: any(line.startswith(marker + " ") for line in lines)
        for marker in roadmap_markers
    }
    coverage = coverage_statuses(arch, fields)
    with summary.open("w", encoding="utf-8") as handle:
        handle.write(f"arch={arch}\n")
        handle.write(f"kernel={kernel}\n")
        handle.write(f"qemu_returncode={qemu_returncode}\n")
        handle.write(f"validation_code={validation_code}\n")
        handle.write(f"capabilities={','.join(CAPABILITIES_BY_ARCH.get(arch, ()))}\n")
        handle.write(f"ok_mode={'OK_MODE debug' in lines}\n")
        handle.write(f"boot_complete={any(line.startswith('OK_DEBUG boot=complete') for line in lines)}\n")
        handle.write(f"ok_test_pass={bool(pass_lines)}\n")
        if fields:
            for key in sorted(fields):
                handle.write(f"field.{key}={fields[key]}\n")
        for field in ALL_COVERAGE_FIELDS:
            handle.write(f"coverage.{field}={coverage[field]}\n")
            if coverage[field] == "skip":
                handle.write(f"skip.{field}=missing_capability\n")
        for marker, present in marker_status.items():
            handle.write(f"marker.{marker}={'present' if present else 'missing'}\n")


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--kernel", required=True, type=Path, help="Path to the compiled kernel.bin")
    parser.add_argument("--display", default="none")
    parser.add_argument("--no-debug-exit", action="store_true")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    arch = normalize_arch(args.arch)
    kernel = args.kernel.resolve()
    if not kernel.exists():
        print(f"kernel image does not exist: {kernel}", file=sys.stderr)
        return 2

    use_debug_exit = not args.no_debug_exit and arch in ("i386", "x86_64")
    try:
        result = run_kernel(arch, kernel, args.display, use_debug_exit, args.timeout)
    except QemuConfigurationError as error:
        print(str(error), file=sys.stderr)
        write_summary(kernel, arch, "", 127, 11)
        return 11
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        if stdout:
            print(stdout, end="")
        if stderr:
            print(stderr, end="", file=sys.stderr)
        print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
        write_summary(kernel, arch, stdout, 124, 12)
        return 12
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    validation_code = validate_output(arch, result.stdout, result.returncode, use_debug_exit)
    write_summary(kernel, arch, result.stdout, result.returncode, validation_code)
    return validation_code


if __name__ == "__main__":
    raise SystemExit(main())
