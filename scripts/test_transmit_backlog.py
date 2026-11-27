#!/usr/bin/env python3
"""Hosted model test of the datagram transmit backlog (smp-scaling WP5, spec 4.13 try-transmit).

Model coverage only: no NIC, driver or guest executes.
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
    with tempfile.TemporaryDirectory(prefix="ok-transmit-backlog-") as directory:
        output = Path(directory) / "transmit-backlog"
        # A sanitizer report must fail the run, not just print.
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", *flags, f"-I{root / 'include'}", f"-I{root / 'src'}",
            str(root / "tests/host/transmit_backlog_tests.cpp"),
            "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
