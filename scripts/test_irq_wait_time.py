#!/usr/bin/env python3
"""Hosted model test for IrqRouter, WaitQueue and CalibratedClock (feat/irq-wait-time).

Model coverage only: no interrupt controller, device or hardware timer executes.
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
    with tempfile.TemporaryDirectory(prefix="ok-irq-wait-time-") as directory:
        output = Path(directory) / "irq-wait-time"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", *flags, f"-I{root / 'include'}", f"-I{root / 'src'}",
            str(root / "tests/host/irq_wait_time_tests.cpp"), str(root / "src/interrupt/interrupt.cpp"),
            str(root / "src/interrupt/platform_irq.cpp"),
            "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
