#!/usr/bin/env python3
"""Hosted model test for ranked spin locks, gates and their debug checks (feat/smp-bringup, smp-scaling WP0a).

Model coverage only: host threads stand in for CPUs; no AP, interrupt mask or IPI executes.
The lock suite is built with OK_LOCK_DEBUG and without it (release builds compile the checks
out), each with OK_HOSTED_LOCK_MODEL (FineLock is the real spin lock) and without it (FineLock
is an UpLock). The gate suite races ServingGate and SharedGate on host threads, so it is built
with OK_HOSTED_LOCK_MODEL only, with and without OK_LOCK_DEBUG.
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def build_and_run(args, root, directory, name, source, defines):
    output = Path(directory) / name
    # A sanitizer report must fail the run, not just print.
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"] if args.sanitize else []
    subprocess.run([
        *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-fno-exceptions", "-pthread", *flags, *defines, f"-I{root / 'include'}", f"-I{root / 'src'}",
        str(root / source), str(root / "src/arch/x86_64/ops.cpp"),
        "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, help="keep the binaries here instead of a temporary directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    model = "-DOK_HOSTED_LOCK_MODEL"
    with tempfile.TemporaryDirectory(prefix="ok-smp-locks-") as scratch:
        directory = args.output or Path(scratch)
        directory.mkdir(parents=True, exist_ok=True)
        locks = "tests/host/smp_lock_tests.cpp"
        build_and_run(args, root, directory, "smp-locks-release", locks, [model])
        build_and_run(args, root, directory, "smp-locks-debug", locks, [model, "-DOK_LOCK_DEBUG"])
        build_and_run(args, root, directory, "smp-locks-up-release", locks, [])
        build_and_run(args, root, directory, "smp-locks-up-debug", locks, ["-DOK_LOCK_DEBUG"])
        gates = "tests/host/smp_gate_tests.cpp"
        build_and_run(args, root, directory, "smp-gates-release", gates, [model])
        build_and_run(args, root, directory, "smp-gates-debug", gates, [model, "-DOK_LOCK_DEBUG"])


if __name__ == "__main__":
    main()
