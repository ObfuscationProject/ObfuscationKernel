#!/usr/bin/env python3
"""Boot kernel.bin in a QEMU window and report after the window closes."""

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
BOOTABLE_QEMU_ARCHES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32")
BOOTABLE_QEMU_CAPABILITIES = (
    "serial_console", "framebuffer", "keyboard_input", "mouse_input", "pci_bus",
    "virtio_block", "virtio_gpu", "ramfb", "usb_hid", "network_loopback",
)
PROFILE_CAPABILITIES = ("serial_console", "network_loopback")

CAPABILITIES_BY_ARCH = {arch: BOOTABLE_QEMU_CAPABILITIES for arch in BOOTABLE_QEMU_ARCHES}
CAPABILITIES_BY_ARCH.update({
    "loongarch64": PROFILE_CAPABILITIES,
    "mips": PROFILE_CAPABILITIES,
    "mips64": PROFILE_CAPABILITIES,
    "ppc": PROFILE_CAPABILITIES,
})


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


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def arch_capabilities(arch: str) -> set[str]:
    return set(CAPABILITIES_BY_ARCH.get(arch, ()))


def required_coverage_fields(arch: str) -> set[str]:
    capabilities = arch_capabilities(arch)
    fields = set(BASE_COVERAGE_FIELDS)
    for field, required_caps in CAPABILITY_COVERAGE_FIELDS.items():
        if any(capability in capabilities for capability in required_caps):
            fields.add(field)
    return fields


def validate_output(arch: str, output: str) -> tuple[bool, str]:
    lines = output.splitlines()
    if "OK_MODE debug" not in lines:
        return False, "missing OK_MODE debug"
    if not any(line.startswith("OK_DEBUG boot=complete") for line in lines):
        return False, "missing boot completion"
    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    if not pass_lines:
        return False, "missing OK_TEST_PASS"
    fields = parse_fields(pass_lines[-1])
    if fields.get("arch") != arch:
        return False, f"arch mismatch: expected {arch}, got {fields.get('arch')}"
    for required in sorted(required_coverage_fields(arch)):
        if fields.get(required) != "1":
            return False, f"{required} did not pass"
    if fields.get("display") == "1" and int(fields.get("display_checksum", "0")) == 0:
        return False, "display checksum was zero"
    if int(fields.get("debug_test_points", "0")) == 0:
        return False, "debug test points did not run"
    return True, fields.get("debug_test_points", "0")


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


def qemu_command(arch: str, kernel: Path, display: str, disk: Path) -> list[str]:
    qemu = QEMU_SYSTEM_BY_ARCH.get(arch)
    if qemu is None:
        raise SystemExit(f"qemu-system boot is not implemented for {arch} yet")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise SystemExit(f"{qemu} was not found in PATH")
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
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
            "-device",
            "ramfb",
            "-device",
            "virtio-keyboard-device",
            "-device",
            "virtio-mouse-device",
        ]
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
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
            "-device",
            "ramfb",
            "-device",
            "virtio-keyboard-device",
            "-device",
            "virtio-mouse-device",
        ]
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
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
            "-device",
            "ramfb",
            "-device",
            "virtio-keyboard-pci",
            "-device",
            "virtio-mouse-pci",
        ]
        command += virtio_disk_args(disk)
        return command
    if arch in ("mips", "mips64"):
        command = [
            qemu_path,
            "-M",
            "malta",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
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
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
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
        "-device",
        "ramfb",
    ]
    command += virtio_disk_args(disk)
    return command

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--display", default="gtk")
    parser.add_argument("--no-launch", action="store_true", help="Run headless but keep the window-test validation path")
    args = parser.parse_args()

    arch = normalize_arch(args.arch)
    kernel = args.kernel.resolve()
    if not kernel.exists():
        print(f"kernel image does not exist: {kernel}", file=sys.stderr)
        return 2

    display = "none" if args.no_launch else args.display
    with tempfile.TemporaryDirectory(prefix="okernel-qemu-window-") as tmp:
        runnable_kernel = Path(tmp) / "kernel.bin"
        scratch_disk = Path(tmp) / "fs.img"
        shutil.copyfile(kernel, runnable_kernel)
        create_virtual_disk(scratch_disk)
        command = qemu_command(arch, runnable_kernel, display, scratch_disk)
        if args.no_launch and arch in ("i386", "x86_64"):
            command += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
            timeout = 10.0
        elif args.no_launch:
            timeout = 10.0
        else:
            timeout = None
        if args.no_launch and arch not in ("i386", "x86_64"):
            result = run_until_marker(command, timeout)
        else:
            result = subprocess.run(command, text=True, capture_output=True, check=False, timeout=timeout)
    ok, detail = validate_output(arch, result.stdout)
    if ok:
        print(f"OK_QEMU_NET_TEST arch={arch} udp=pass tcp=pass")
        print(f"QEMU_WINDOW_TEST_PASS arch={arch} debug_test_points={detail} kernel={kernel}")
        return 0
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    print(f"QEMU_WINDOW_TEST_FAIL arch={arch} reason={detail} returncode={result.returncode} kernel={kernel}")
    return result.returncode if result.returncode != 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
