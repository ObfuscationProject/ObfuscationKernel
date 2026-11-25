#!/usr/bin/env python3
"""Exercise the exec argument arena, #! interpreters and ENOEXEC with a hosted backend.

Built once per architecture profile (OK_ARCH_TARGET_<ARCH> with that profile's ops.cpp; --arch
selects one). Hosted model only: no guest execution.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ARCHES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arch", choices=("all", *ARCHES), default="all")
    args = parser.parse_args()
    arches = ARCHES if args.arch == "all" else (args.arch,)
    root = Path(__file__).resolve().parents[1]
    sources = [root / "tests/host/native_exec_arguments_tests.cpp"]
    for directory in ("core", "rescue", "driver", "fs", "gui", "interrupt", "ipc", "memory",
                      "net", "posix", "sched", "smp", "syscall", "user"):
        sources.extend(path for path in sorted((root / "src" / directory).rglob("*.cpp"))
                       if path.name not in ("entry.cpp", "kernel_main.cpp", "runtime.cpp") and path.parent.name != 'runtime')
    sources.extend(root / path for path in ("src/arch/arch.cpp", "src/arch/terminal_power.cpp"))
    with tempfile.TemporaryDirectory(prefix="ok-native-exec-arguments-") as directory:
        for arch in arches:
            executable = Path(directory) / f"services-{arch}"
            subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra",
                            "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections",
                            "-Wl,--gc-sections",
                            *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []),
                            f"-DOK_ARCH_TARGET_{arch.upper()}", "-I", str(root / "include"),
                            *(str(path) for path in sources), str(root / f"src/arch/{arch}/ops.cpp"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            print(f"PASS {arch} hosted native exec arguments", flush=True)


if __name__ == "__main__":
    main()
