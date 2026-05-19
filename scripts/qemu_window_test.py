#!/usr/bin/env python3
"""Show the debug kernel display output in a standalone QEMU window."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


PASS_RE = re.compile(r"OK_TEST_PASS arch=([^ ]+).*debug_test_points=([0-9]+)")


class BootSector:
    def __init__(self) -> None:
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str]] = []

    def emit(self, data: bytes) -> None:
        self.code.extend(data)

    def label(self, name: str) -> None:
        self.labels[name] = len(self.code)

    def jmp_short(self, opcode: int, label: str) -> None:
        self.emit(bytes((opcode, 0)))
        self.fixups.append((len(self.code) - 1, label))

    def build(self, text: str) -> bytes:
        self.emit(b"\x31\xc0\x8e\xd8")
        si_fixup = len(self.code) + 1
        self.emit(b"\xbe\x00\x00")
        self.label("loop")
        self.emit(b"\xac\x84\xc0")
        self.jmp_short(0x74, "halt")
        self.emit(b"\x3c\x0a")
        self.jmp_short(0x75, "print")
        self.emit(b"\xb4\x0e\xb0\x0d\xcd\x10\xb0\x0a\xcd\x10")
        self.jmp_short(0xeb, "loop")
        self.label("print")
        self.emit(b"\xb4\x0e\xbb\x07\x00\xcd\x10")
        self.jmp_short(0xeb, "loop")
        self.label("halt")
        self.emit(b"\xf4\xeb\xfd")

        message_offset = len(self.code)
        message_address = 0x7c00 + message_offset
        self.code[si_fixup] = message_address & 0xff
        self.code[si_fixup + 1] = (message_address >> 8) & 0xff

        for fixup, label in self.fixups:
            relative = self.labels[label] - (fixup + 1)
            if relative < -128 or relative > 127:
                raise RuntimeError("boot sector branch is out of range")
            self.code[fixup] = relative & 0xff

        payload = text.encode("ascii", errors="replace")[: 510 - len(self.code) - 1]
        self.emit(payload + b"\x00")
        if len(self.code) > 510:
            raise RuntimeError("QEMU window boot sector text is too large")
        self.emit(bytes(510 - len(self.code)))
        self.emit(b"\x55\xaa")
        return bytes(self.code)


def run_current_kernel(root: Path) -> tuple[str, str, int]:
    build = subprocess.run(["xmake", "-y", "-b", "qemu_kernel"], cwd=root, text=True, capture_output=True, check=False)
    if build.returncode != 0:
        if build.stdout:
            print(build.stdout, end="")
        if build.stderr:
            print(build.stderr, end="")
        raise subprocess.CalledProcessError(build.returncode, ["xmake", "-y", "-b", "qemu_kernel"])
    result = subprocess.run(["xmake", "run", "qemu_kernel"], cwd=root, text=True, capture_output=True, check=False)
    return result.stdout, result.stderr, result.returncode


def kernel_display_text(kernel_output: str) -> str:
    display_lines = [
        line.removeprefix("OK_DISPLAY_TEXT ")
        for line in kernel_output.splitlines()
        if line.startswith("OK_DISPLAY_TEXT ")
    ]
    if display_lines:
        return "\n".join(display_lines[:12])
    return "[    0.000000] okernel: no framebuffer text reported\n"


def print_result(arch: str, stdout: str, stderr: str, returncode: int, image: Path) -> int:
    match = PASS_RE.search(stdout)
    if returncode == 0 and match:
        print(f"QEMU_WINDOW_TEST_PASS arch={arch} debug_test_points={match.group(2)} image={image}")
        return 0

    if stdout:
        print(stdout, end="")
    if stderr:
        print(stderr, end="")
    print(f"QEMU_WINDOW_TEST_FAIL arch={arch} returncode={returncode} image={image}")
    return returncode if returncode != 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--display", default="gtk", help="QEMU display backend, for example gtk or sdl")
    parser.add_argument("--no-launch", action="store_true", help="Only generate the demo image")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    kernel_stdout, kernel_stderr, kernel_returncode = run_current_kernel(root)
    sector = BootSector().build(kernel_display_text(kernel_stdout))

    out_dir = Path(tempfile.mkdtemp(prefix="ok-qemu-window-"))
    image = out_dir / "ok-window-demo.img"
    image.write_bytes(sector + bytes(1474560 - len(sector)))

    if args.no_launch:
        return print_result(args.arch, kernel_stdout, kernel_stderr, kernel_returncode, image)

    qemu = shutil.which("qemu-system-x86_64") or shutil.which("qemu-system-i386")
    if qemu is None:
        raise FileNotFoundError("qemu-system-x86_64 or qemu-system-i386 was not found in PATH")

    qemu_result = subprocess.run(
        [
            qemu,
            "-drive",
            f"file={image},format=raw,if=floppy",
            "-boot",
            "a",
            "-display",
            args.display,
            "-no-reboot",
            "-name",
            "ObfuscationKernel debug kernel",
        ],
        cwd=root,
        check=False,
    )
    if qemu_result.returncode != 0:
        print(f"QEMU_WINDOW_TEST_FAIL arch={args.arch} qemu_returncode={qemu_result.returncode} image={image}")
        return qemu_result.returncode
    return print_result(args.arch, kernel_stdout, kernel_stderr, kernel_returncode, image)


if __name__ == "__main__":
    raise SystemExit(main())
