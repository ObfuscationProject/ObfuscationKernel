#!/usr/bin/env python3
"""Boot kernel.bin in a QEMU window and report after the window closes."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


QEMU_SYSTEM_BY_ARCH = {
    "i386": "qemu-system-i386",
    "x86_64": "qemu-system-x86_64",
    "aarch64": "qemu-system-aarch64",
}


def normalize_arch(arch: str) -> str:
    return {"x64": "x86_64"}.get(arch, arch)


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
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
    for required in ("fs", "ext4", "user", "display", "input", "modes"):
        if fields.get(required) != "1":
            return False, f"{required} did not pass"
    if int(fields.get("debug_test_points", "0")) == 0:
        return False, "debug test points did not run"
    return True, fields.get("debug_test_points", "0")


def qemu_command(arch: str, kernel: Path, display: str) -> list[str]:
    qemu = QEMU_SYSTEM_BY_ARCH.get(arch)
    if qemu is None:
        raise SystemExit(f"qemu-system boot is not implemented for {arch} yet")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise SystemExit(f"{qemu} was not found in PATH")
    if arch == "aarch64":
        return [
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

    return [
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
        shutil.copyfile(kernel, runnable_kernel)
        command = qemu_command(arch, runnable_kernel, display)
        if args.no_launch and arch in ("i386", "x86_64"):
            command += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
            timeout = 10.0
        elif args.no_launch:
            timeout = 10.0
        else:
            timeout = None
        try:
            result = subprocess.run(command, text=True, capture_output=True, check=False, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            stdout = exc.stdout or ""
            stderr = exc.stderr or "qemu marker timeout\n"
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")
            result = subprocess.CompletedProcess(command, 0 if "OK_TEST_PASS " in stdout else 124, stdout, stderr)
    ok, detail = validate_output(arch, result.stdout)
    if ok:
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
