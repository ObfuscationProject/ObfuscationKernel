#!/usr/bin/env python3
"""Verify the ppce500 post-boot TLB extents and permissions in QEMU."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import time
from pathlib import Path


def tlb_entries(monitor: str) -> dict[int, list[tuple[int, str, str, str, str]]]:
    entries: dict[int, list[tuple[int, str, str, str, str]]] = {}
    if "TLB1:" not in monitor:
        raise ValueError("QEMU monitor did not report TLB1")
    for line in monitor.split("TLB1:", 1)[1].splitlines():
        fields = line.split()
        if len(fields) < 9 or not fields[0].startswith("0x"):
            continue
        try:
            effective, physical = int(fields[0], 16), int(fields[1], 16)
        except ValueError:
            continue
        entries.setdefault(effective, []).append(
            (physical, fields[2], fields[5], fields[6], fields[7])
        )
    return entries


def validate(monitor: str) -> None:
    entries = tlb_entries(monitor)
    expected = {
        0x00000000: (0x00000000, "256M", "SRWX", "U---", "-----"),
        0xC0000000: (0xC0000000, "256M", "SRW-", "U---", "-I-G-"),
        0xE0000000: (0xFE0000000, "1M", "SRWX", "U---", "-I-G-"),
    }
    for address, value in expected.items():
        if entries.get(address) != [value]:
            raise ValueError(f"TLB1 entry {address:#010x} differs: {entries.get(address)}")


def run(qemu: Path, kernel: Path, timeout: float, output: Path | None) -> None:
    with tempfile.TemporaryDirectory(prefix="ok-ppc-tlb-") as temporary:
        serial_path = Path(temporary) / "serial.log"
        command = [str(qemu), "-M", "ppce500", "-m", "256M", "-kernel", str(kernel),
                   "-display", "none", "-serial", f"file:{serial_path}",
                   "-monitor", "stdio", "-no-reboot"]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        monitor = ""
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if serial_path.exists() and b"OK_TEST_PASS arch=ppc" in serial_path.read_bytes():
                    break
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited before PPC boot marker: {process.returncode}")
                time.sleep(0.05)
            else:
                raise TimeoutError("PPC boot marker did not arrive")
            monitor, _ = process.communicate("info tlb\nquit\n", timeout=10)
            validate(monitor)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            if output is not None:
                output.mkdir(parents=True, exist_ok=True)
                (output / "serial.log").write_bytes(serial_path.read_bytes() if serial_path.exists() else b"")
                (output / "monitor.log").write_text(monitor)
        if output is not None:
            (output / "result.json").write_text(json.dumps({
                "scope": "qemu-ppce500-post-boot-tlb",
                "kernel": str(kernel),
                "qemu": str(qemu),
                "ram": "256M supervisor-only",
                "pci": "256M supervisor-only cache-inhibited guarded",
                "ccsr": "1M supervisor-only cache-inhibited guarded",
            }, indent=2) + "\n")
    print("OK_PPC_TLB scope=qemu ram=256M pci=256M ccsr=1M supervisor_only=pass")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    for name in ("qemu", "kernel"):
        path = getattr(args, name).resolve()
        if not path.is_file():
            parser.error(f"{name} is not a file: {path}")
        setattr(args, name, path)
    run(args.qemu, args.kernel, args.timeout, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
