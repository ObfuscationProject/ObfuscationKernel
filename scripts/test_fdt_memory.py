#!/usr/bin/env python3
"""Test bounded FDT parsing; optionally audit explicit dumped DTB files."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--dtb", type=Path, action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-fdt-memory-") as directory:
        output = Path(directory) / "fdt-memory"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-fno-exceptions", *flags,
            f"-I{root / 'include'}", str(root / "tests/host/fdt_memory_tests.cpp"),
            str(root / "src/boot/fdt.cpp"), "-o", str(output),
        ], check=True)
        subprocess.run([str(output), *(str(path.resolve()) for path in args.dtb)], check=True)


if __name__ == "__main__":
    main()
