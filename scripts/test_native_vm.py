#!/usr/bin/env python3
"""Test native VM transactions with a hosted, fault-injectable execution backend.

Built once per architecture profile (OK_ARCH_TARGET_<ARCH>; --arch selects one), each with and
without copy-on-write fork. Hosted model only: no guest, hardware or SMP execution.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ARCHES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arch", choices=("all", *ARCHES), default="all")
    args = parser.parse_args()
    arches = ARCHES if args.arch == "all" else (args.arch,)
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-native-vm-") as directory:
        # The included runtime also contains unrelated virtual-device call sites;
        # their vptr metadata would retain driver RTTI after section collection.
        flags = ["-fsanitize=address,undefined", "-fno-sanitize=vptr",
                 "-fno-omit-frame-pointer"] if args.sanitize else []
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
        # Copy-on-write fork is the default build; native_cow_fork=n keeps eager copies.
        for arch in arches:
            for name, defines in (("native-vm", ["-DOK_NATIVE_COW_FORK"]), ("native-vm-eager", [])):
                output = Path(directory) / f"{name}-{arch}"
                subprocess.run([
                    *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *flags,
                    f"-DOK_ARCH_TARGET_{arch.upper()}", *defines, f"-I{root / 'include'}",
                    str(root / "tests/host/native_vm_tests.cpp"), str(root / "src/sched/process.cpp"),
                    str(root / "src/memory/memory.cpp"), "-o", str(output),
                ], check=True)
                subprocess.run([str(output)], check=True, env=environment)
                subprocess.run([str(output), "--modeled-qualified-pool"], check=True, env=environment)
            print(f"PASS {arch} hosted native VM transactions", flush=True)


if __name__ == "__main__":
    main()
