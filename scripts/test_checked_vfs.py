#!/usr/bin/env python3
"""Exercise checked metadata/lookup, cleanup, and mutation admission on every profile."""
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
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = ["tests/host/checked_vfs_tests.cpp", "src/sched/scheduler.cpp", "src/sched/process.cpp", "src/posix/posix.cpp", "src/posix/fd.cpp", "src/posix/path.cpp", "src/posix/process.cpp", "src/fs/vfs.cpp", "src/driver/driver.cpp", "src/smp/smp.cpp", "src/arch/arch.cpp", "src/syscall/linux.cpp", "src/memory/memory.cpp", "src/net/native_udp.cpp"]
    with tempfile.TemporaryDirectory(prefix="ok-checked-vfs-") as directory:
        for arch in ARCHITECTURES if args.arch == "all" else (args.arch,):
            output = Path(directory) / arch
            command = [*shlex.split(args.cxx), "-std=c++23", "-O1" if args.sanitize else "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []), f"-I{root / 'include'}", f"-DOK_ARCH_TARGET_{arch.upper()}", *(str(root / name) for name in sources), str(root / f"src/arch/{arch}/ops.cpp"), "-o", str(output)]
            subprocess.run(command, check=True)
            subprocess.run([str(output)], check=True)
            print(f"PASS {arch}: checked-vfs ownership model", flush=True)

if __name__ == "__main__":
    main()
