#!/usr/bin/env python3
"""Exercise descriptor readiness and nonblocking service models on every profile."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ARCHITECTURES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("all", *ARCHITECTURES), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = ["tests/host/fd_readiness_tests.cpp", "src/sched/scheduler.cpp", "src/sched/process.cpp", "src/posix/posix.cpp", "src/posix/fd.cpp", "src/posix/path.cpp", "src/posix/process.cpp", "src/fs/vfs.cpp", "src/driver/driver.cpp", "src/smp/smp.cpp", "src/arch/arch.cpp", "src/syscall/linux.cpp"]
    with tempfile.TemporaryDirectory(prefix="ok-fd-readiness-") as directory:
        for arch in ARCHITECTURES if args.arch == "all" else (args.arch,):
            output = Path(directory) / arch
            command = [*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", f"-I{root / 'include'}", f"-DOK_ARCH_TARGET_{arch.upper()}", *(str(root / name) for name in sources), str(root / f"src/arch/{arch}/ops.cpp"), "-o", str(output)]
            subprocess.run(command, check=True)
            subprocess.run([str(output)], check=True)
            print(f"PASS {arch}: descriptor readiness model", flush=True)

if __name__ == "__main__":
    main()
