#!/usr/bin/env python3
"""Hosted boot memory tests: EFI/PROM decoders, loader discovery, kernel plans and maps.

Bookkeeping and decoding only; QEMU boots of the ten profiles cover real discovery.
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-boot-memory-") as directory:
        output = Path(directory) / "boot-memory"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", *flags, f"-I{root / 'include'}",
            str(root / "tests/host/boot_memory_tests.cpp"), str(root / "src/boot/fdt.cpp"),
            str(root / "src/memory/physical_map.cpp"), str(root / "src/memory/memory.cpp"),
            "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
