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
    "rv64": "qemu-system-riscv64",
}

QEMU_DEBUG_EXIT_SUCCESS = 33
VIRTUAL_DISK_SIZE = 16 * 1024 * 1024


def normalize_arch(arch: str) -> str:
    aliases = {
        "x64": "x86_64",
    }
    return aliases.get(arch, arch)


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


def qemu_command(arch: str, kernel: Path, display: str, debug_exit: bool, disk: Path) -> list[str]:
    qemu = QEMU_SYSTEM_BY_ARCH.get(arch)
    if qemu is None:
        raise SystemExit(f"qemu-system boot is not implemented for {arch} yet")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise SystemExit(f"{qemu} was not found in PATH")

    if arch == "aarch64":
        command = [
            qemu_path,
            "-M",
            "virt",
            "-cpu",
            "cortex-a57",
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
    if arch == "rv64":
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
        "-display",
        display,
    ]
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
        print(f"qemu did not exit through debug-exit success path: returncode={returncode}", file=sys.stderr)
        return returncode if returncode != 0 else 11
    if not accept_debug_exit and returncode != 0:
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
    for required in ("fs", "simplefs", "ext4", "user", "display", "gpu", "input", "posix", "bus", "usb", "net", "shell", "modes"):
        if fields.get(required) != "1":
            print(f"debug kernel did not pass {required} test coverage", file=sys.stderr)
            return 8
    if int(fields.get("display_checksum", "0")) == 0:
        print("display driver did not produce a framebuffer checksum", file=sys.stderr)
        return 9
    if not any(line.startswith("OK_DISPLAY_TEXT ") for line in lines):
        print("display driver did not report boot text", file=sys.stderr)
        return 10
    return 0


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
    result = run_kernel(arch, kernel, args.display, use_debug_exit, args.timeout)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return validate_output(arch, result.stdout, result.returncode, use_debug_exit)


if __name__ == "__main__":
    raise SystemExit(main())
