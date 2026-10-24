#!/usr/bin/env python3
"""Exercise bounded physical frame bookkeeping; this is not a hardware VM test."""
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
        output = Path(directory) / "frame-allocator"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-fno-exceptions", *flags,
            f"-I{root / 'include'}", str(root / "tests/host/frame_allocator_tests.cpp"),
            str(root / "src/memory/memory.cpp"), "-o", str(output),
        ], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
