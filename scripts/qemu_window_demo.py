#!/usr/bin/env python3
"""Show architecture smoke status in a standalone QEMU graphical window."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ARCHES = ("host", "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64")
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
        self.emit(b"\x31\xc0\x8e\xd8")  # xor ax, ax; mov ds, ax
        si_fixup = len(self.code) + 1
        self.emit(b"\xbe\x00\x00")  # mov si, message
        self.label("loop")
        self.emit(b"\xac\x84\xc0")  # lodsb; test al, al
        self.jmp_short(0x74, "halt")
        self.emit(b"\x3c\x0a")  # cmp al, 0x0a
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


def run_arch_check(root: Path, mode: str) -> dict[str, str]:
    process = subprocess.Popen(
        ["python3", str(root / "scripts" / "arch_check.py"), "--mode", mode],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output: list[str] = []
    if process.stdout is not None:
        for line in process.stdout:
            output.append(line)
            print(line, end="")
    return_code = process.wait()
    if return_code != 0:
        raise subprocess.CalledProcessError(return_code, ["arch_check"])

    status = {arch: "MISSING" for arch in ARCHES}
    for line in output:
        match = PASS_RE.search(line)
        if match:
            status[match.group(1)] = f"PASS tp={match.group(2)}"
    return status


def demo_text(status: dict[str, str]) -> str:
    lines = ["ObfuscationKernel QEMU Window Test", ""]
    for arch in ARCHES:
        lines.append(f"{arch:<12} {status.get(arch, 'MISSING')}")
    lines.extend(["", "Close the QEMU window or press Ctrl-A X."])
    return "\n".join(lines)


def restore(root: Path, arch: str | None, toolchain: str | None, mode: str) -> None:
    if not arch:
        return
    command = ["xmake", "f", "-c", "-m", mode, f"--arch_target={arch}"]
    if toolchain:
        command.append(f"--toolchain={toolchain}")
    subprocess.run(command, cwd=root, check=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--display", default="gtk", help="QEMU display backend, for example gtk or sdl")
    parser.add_argument("--no-launch", action="store_true", help="Only generate the boot image")
    parser.add_argument("--restore-arch")
    parser.add_argument("--restore-toolchain")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    try:
        status = run_arch_check(root, args.mode)
        sector = BootSector().build(demo_text(status))

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
    finally:
        restore(root, args.restore_arch, args.restore_toolchain or None, args.mode)


if __name__ == "__main__":
    raise SystemExit(main())
