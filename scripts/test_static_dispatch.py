#!/usr/bin/env python3
"""Compile and run host-side dispatch/model tests for every kernel ISA profile.

This checks C++ profile selection and scheduling policy behavior. It deliberately
makes no claim about boot, MMU, hardware interrupts, context switching, or SMP.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ARCHITECTURES = (
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64",
    "mips", "mips64", "ppc",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"), help="Host C++ compiler command")
    parser.add_argument("--arch", choices=("all", *ARCHITECTURES), default="all")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    architectures = ARCHITECTURES if args.arch == "all" else (args.arch,)
    with tempfile.TemporaryDirectory(prefix="ok-static-dispatch-") as output:
        for architecture in architectures:
            executable = Path(output) / architecture
            command = [
                *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", f"-I{root / 'include'}",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                f"-DOK_ARCH_TARGET_{architecture.upper()}",
                str(root / "tests/host/static_dispatch_tests.cpp"),
                str(root / "src/sched/scheduler.cpp"),
                str(root / "src/sched/process.cpp"),
                str(root / "src/memory/memory.cpp"),
                str(root / "src/net/native_udp.cpp"),
                str(root / "src/fs/vfs.cpp"),
                str(root / "src/arch/arch.cpp"),
                str(root / f"src/arch/{architecture}/ops.cpp"),
                "-o", str(executable),
            ]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
            print(f"PASS {architecture}: static architecture dispatch and two scheduler policies", flush=True)


if __name__ == "__main__":
    main()
