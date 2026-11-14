#!/usr/bin/env python3
"""Hosted model test for ranked spin locks and their debug order checks (feat/smp-bringup).

Model coverage only: host threads stand in for CPUs; no AP, interrupt mask or IPI executes.
The suite is built with OK_LOCK_DEBUG and again without it, to show release builds compile the
checks out.
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def build_and_run(args, root, directory, name, defines):
    output = Path(directory) / name
    # A sanitizer report must fail the run, not just print.
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"] if args.sanitize else []
    subprocess.run([
        *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-fno-exceptions", "-pthread", *flags, *defines, f"-I{root / 'include'}", f"-I{root / 'src'}",
        str(root / "tests/host/smp_lock_tests.cpp"), str(root / "src/arch/x86_64/ops.cpp"),
        "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, help="keep the binaries here instead of a temporary directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-smp-locks-") as scratch:
        directory = args.output or Path(scratch)
        directory.mkdir(parents=True, exist_ok=True)
        build_and_run(args, root, directory, "smp-locks-release", [])
        build_and_run(args, root, directory, "smp-locks-debug", ["-DOK_LOCK_DEBUG"])


if __name__ == "__main__":
    main()
