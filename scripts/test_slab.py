#!/usr/bin/env python3
"""Hosted slab allocator test over host-backed RAM: size classes, charges, contexts, reclaim, large objects.

This exercises bookkeeping on the host; it is not RAM discovery or a hardware VM test. A second
build defines OK_HOSTED_LOCK_MODEL and OK_LOCK_DEBUG and races host threads standing in for CPUs
(scope=model).
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
    with tempfile.TemporaryDirectory(prefix="ok-slab-") as directory:
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        for name, defines in (("slab", []),
                              ("slab-race", ["-DOK_HOSTED_LOCK_MODEL", "-DOK_LOCK_DEBUG", "-pthread"])):
            output = Path(directory) / name
            subprocess.run([
                *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-fno-exceptions", *flags, *defines, f"-I{root / 'include'}",
                str(root / "tests/host/slab_tests.cpp"), str(root / "src/memory/slab.cpp"),
                str(root / "src/memory/physical_map.cpp"), str(root / "src/memory/memory.cpp"),
                "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
