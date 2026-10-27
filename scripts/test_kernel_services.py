#!/usr/bin/env python3
"""Run actual kernel GUI/service boundaries with the host execution stub."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = [root / "tests/host/kernel_service_tests.cpp"]
    for directory in ("core", "rescue", "driver", "fs", "gui", "interrupt", "ipc", "memory",
                      "net", "posix", "sched", "smp", "syscall", "user"):
        sources.extend(path for path in sorted((root / "src" / directory).rglob("*.cpp"))
                       if path.name not in ("entry.cpp", "kernel_main.cpp") or path.parent.name == "runtime")
    sources.extend(root / path for path in ("src/arch/arch.cpp", "src/arch/clock.cpp", "src/arch/realtime.cpp",
                                          "src/arch/terminal_power.cpp", "src/arch/x86_64/ops.cpp",
                                          "src/arch/execution_stub.cpp"))
    with tempfile.TemporaryDirectory(prefix="ok-kernel-services-") as directory:
        executable = Path(directory) / "services"
        sanitize_flags = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
                          if args.sanitize else [])
        subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", *sanitize_flags,
                        "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-DOK_ARCH_TARGET_X86_64", "-I", str(root / "include"),
                        *(str(path) for path in sources), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
