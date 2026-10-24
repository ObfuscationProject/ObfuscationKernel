#!/usr/bin/env python3
"""Exercise actual native poll registration with a hosted copy backend."""
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
    sources = [root / "tests/host/native_poll_tests.cpp"]
    for directory in ("core", "apps", "driver", "fs", "gui", "interrupt", "ipc", "memory",
                      "net", "posix", "sched", "smp", "syscall", "user"):
        sources.extend(path for path in sorted((root / "src" / directory).rglob("*.cpp"))
                       if path.name not in ("entry.cpp", "kernel_main.cpp", "runtime.cpp"))
    sources.extend(root / path for path in ("src/arch/arch.cpp", "src/arch/x86_64/ops.cpp"))
    with tempfile.TemporaryDirectory(prefix="ok-native-poll-") as directory:
        executable = Path(directory) / "services"
        subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
                        "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections",
                        *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []),
                        "-DOK_ARCH_TARGET_X86_64", "-I", str(root / "include"),
                        *(str(path) for path in sources), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
