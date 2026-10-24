#!/usr/bin/env python3
"""Exercise malformed native mapping directories in a hosted process."""

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
    with tempfile.TemporaryDirectory(prefix="ok-mapping-view-") as directory:
        executable = Path(directory) / "mapping-view-tests"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        *flags, "-DOK_ARCH_TARGET_X86_64", "-I", str(root / "include"),
                        str(root / "tests/host/mapping_view_tests.cpp"), "-o", str(executable)], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
