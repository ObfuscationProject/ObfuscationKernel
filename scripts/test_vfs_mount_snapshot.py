#!/usr/bin/env python3
"""Run hosted actual-source mount snapshot models, without guest or seal claims."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ARCHITECTURES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32",
                 "loongarch64", "mips", "mips64", "ppc")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("all", *ARCHITECTURES), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = ("tests/host/vfs_mount_snapshot_tests.cpp", "src/fs/vfs.cpp",
               "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/terminal_power.cpp")
    with tempfile.TemporaryDirectory(prefix="ok-vfs-mount-snapshot-") as directory:
        for arch in ARCHITECTURES if args.arch == "all" else (args.arch,):
            output = Path(directory) / arch
            command = [*shlex.split(args.cxx), "-std=c++23", "-O1" if args.sanitize else "-O2",
                       "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-access-control",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
                         if args.sanitize else []),
                       f"-DOK_ARCH_TARGET_{arch.upper()}", f"-I{root / 'include'}",
                       *(str(root / source) for source in sources),
                       str(root / f"src/arch/{arch}/ops.cpp"), "-o", str(output)]
            subprocess.run(command, check=True)
            environment = dict(os.environ)
            if args.sanitize:
                # The app host uses ptrace, which prevents LeakSanitizer.
                environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
                environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            subprocess.run([str(output)], check=True, env=environment)
            print(f"PASS {arch}: hosted mount snapshot groups=6 debug_models=1 guest=0 physical=0 drain=0 seal=0", flush=True)


if __name__ == "__main__":
    main()
