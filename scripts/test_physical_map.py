#!/usr/bin/env python3
"""Hosted model test for PhysicalMemoryMap, charged frame allocation and the boot metadata carve.

This exercises bookkeeping on the host; it is not RAM discovery or a hardware VM test.
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
    with tempfile.TemporaryDirectory(prefix="ok-physical-map-") as directory:
        output = Path(directory) / "physical-map"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", *flags, f"-I{root / 'include'}",
            str(root / "tests/host/physical_map_tests.cpp"),
            str(root / "src/memory/physical_map.cpp"), str(root / "src/memory/memory.cpp"),
            "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
