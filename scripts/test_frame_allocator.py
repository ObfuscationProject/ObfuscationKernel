#!/usr/bin/env python3
"""Exercise bounded physical frame bookkeeping; this is not a hardware VM test.

A second build defines OK_HOSTED_LOCK_MODEL and OK_LOCK_DEBUG: the frame lock is
the real spin lock and host threads stand in for CPUs (scope=model).
"""
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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-frame-allocator-") as directory:
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        for name, defines in (("frame-allocator", []),
                              ("frame-allocator-race", ["-DOK_HOSTED_LOCK_MODEL", "-DOK_LOCK_DEBUG", "-pthread"])):
            output = Path(directory) / name
            subprocess.run([
                *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-fno-exceptions", *flags, *defines,
                f"-I{root / 'include'}", str(root / "tests/host/frame_allocator_tests.cpp"),
                str(root / "src/memory/memory.cpp"), "-o", str(output),
            ], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
