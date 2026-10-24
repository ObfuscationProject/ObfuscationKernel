#!/usr/bin/env python3
"""Layout check for the draft display/input/seat UAPI headers: the C contract is
compiled freestanding for LP64 and ILP32 (-m32), and the C++ runner executes on
the host. Header contract only; no device executes."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    cc = shlex.split(os.environ.get("CC", "gcc"))
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    for flags in ([], ["-m32"]):
        subprocess.run([*cc, "-std=c11", "-ffreestanding", "-fsyntax-only", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        *flags, f"-I{root / 'include'}", str(root / "tests/host/display_input_uapi_layout.c")], check=True)
        print(f"C layout ok for {flags or ['LP64']}")
    with tempfile.TemporaryDirectory(prefix="ok-display-uapi-") as directory:
        output = Path(directory) / "layout"
        subprocess.run([*cxx, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", f"-I{root / 'include'}",
                        str(root / "tests/host/display_input_uapi_tests.cpp"), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
