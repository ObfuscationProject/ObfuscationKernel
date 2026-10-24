#!/usr/bin/env python3
"""Exercise allocator-backed mapping metadata and ownership on hosted RAM."""
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
    with tempfile.TemporaryDirectory(prefix="ok-native-mapping-pages-") as directory:
        binary = Path(directory) / "mapping-pages"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        *flags, "-DOK_ENABLE_TEST_POINTS", f"-I{root / 'include'}", str(root / "tests/host/native_mapping_pages_tests.cpp"),
                        str(root / "src/memory/memory.cpp"), "-o", str(binary)], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(binary)], check=True, env=environment)


if __name__ == "__main__":
    main()
