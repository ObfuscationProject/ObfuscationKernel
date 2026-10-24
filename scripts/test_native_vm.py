#!/usr/bin/env python3
"""Test native VM transactions with a hosted, fault-injectable execution backend."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-native-vm-") as directory:
        output = Path(directory) / "native-vm"
        # The included runtime also contains unrelated virtual-device call sites;
        # their vptr metadata would retain driver RTTI after section collection.
        flags = ["-fsanitize=address,undefined", "-fno-sanitize=vptr",
                 "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *flags,
            "-DOK_ARCH_TARGET_X86_64", f"-I{root / 'include'}",
            str(root / "tests/host/native_vm_tests.cpp"), str(root / "src/sched/process.cpp"), str(root / "src/memory/memory.cpp"),
            "-o", str(output),
        ], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(output)], check=True, env=environment)
        subprocess.run([str(output), "--modeled-qualified-pool"], check=True, env=environment)


if __name__ == "__main__":
    main()
