#!/usr/bin/env python3
"""Show the current architecture smoke result in a standalone QEMU window."""

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


def run_current_smoke(root: Path) -> str:
    subprocess.run(["xmake", "-y", "-b", "qemu_smoke"], cwd=root, check=True)
    result = subprocess.run(["xmake", "run", "qemu_smoke"], cwd=root, text=True, capture_output=True, check=False)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, ["xmake", "run", "qemu_smoke"])
    if "OK_TEST_PASS" not in result.stdout:
        raise RuntimeError("qemu_smoke did not print OK_TEST_PASS")
    return result.stdout.strip()


def demo_text(arch: str, smoke_output: str) -> str:
    match = PASS_RE.search(smoke_output)
    status = "PASS" if match else "FAIL"
    test_points = match.group(2) if match else "0"
    return "\n".join(
        [
            "ObfuscationKernel QEMU Window Test",
            "",
            f"arch:        {arch}",
            f"status:      {status}",
            f"test points: {test_points}",
            "",
            smoke_output[:320],
            "",
            "Close the QEMU window or press Ctrl-A X.",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--mode", choices=("debug", "release"), default="release")
    parser.add_argument("--display", default="gtk", help="QEMU display backend, for example gtk or sdl")
    parser.add_argument("--no-launch", action="store_true", help="Only generate the demo image")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    smoke_output = run_current_smoke(root)
    sector = BootSector().build(demo_text(args.arch, smoke_output))

    out_dir = Path(tempfile.mkdtemp(prefix="ok-qemu-window-"))
    image = out_dir / "ok-window-demo.img"
    image.write_bytes(sector + bytes(1474560 - len(sector)))
    print(f"QEMU_WINDOW_IMAGE {image}")

    if args.no_launch:
        return 0

    qemu = shutil.which("qemu-system-x86_64") or shutil.which("qemu-system-i386")
    if qemu is None:
        raise FileNotFoundError("qemu-system-x86_64 or qemu-system-i386 was not found in PATH")

    subprocess.run(
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
            "ObfuscationKernel smoke status",
        ],
        cwd=root,
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
