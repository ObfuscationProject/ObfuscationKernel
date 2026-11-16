#!/usr/bin/env python3
"""Test the x86_64 TSC offset estimator and correction decision against injected skew and noise.

This is a model test: two simulated CPUs, no TSC is read.
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
    with tempfile.TemporaryDirectory(prefix="ok-tsc-sync-") as directory:
        output = Path(directory) / "tsc-sync"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-fno-exceptions", *flags,
            f"-I{root / 'include'}", str(root / "tests/host/tsc_sync_tests.cpp"),
            "-o", str(output),
        ], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
