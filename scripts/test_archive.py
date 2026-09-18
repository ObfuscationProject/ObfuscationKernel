#!/usr/bin/env python3
"""Run bounded boot-archive and read-only VFS host regressions."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-archive-") as directory:
        output = Path(directory) / "archive"
        sources = ("tests/host/archive_tests.cpp", "src/fs/archive.cpp", "src/fs/vfs.cpp", "src/smp/smp.cpp")
        subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", f"-I{root / 'include'}", *(str(root / source) for source in sources), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)

if __name__ == "__main__":
    main()
