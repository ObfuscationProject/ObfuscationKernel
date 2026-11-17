#!/usr/bin/env python3
"""Model the user-access recovery contract of ok/user/uaccess.hpp (smp-scaling WP3).

A mock backend refuses withheld writes and a mock resolution hook stands in for the native
runtime: bounded retries, the final attempt holding mm, ownerless spaces, with_user_word and the
class A rule. Built with and without OK_LOCK_DEBUG. Model coverage only (scope=model).
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="ok-uaccess-") as directory:
        for name, defines in (("uaccess", []), ("uaccess-debug", ["-DOK_LOCK_DEBUG"])):
            output = Path(directory) / name
            subprocess.run([
                *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-fno-exceptions", *flags, *defines, "-DOK_ARCH_TARGET_X86_64", f"-I{root / 'include'}",
                str(root / "tests/host/uaccess_tests.cpp"), "-o", str(output)], check=True)
            result = subprocess.run([str(output)], check=True, stdout=subprocess.PIPE, text=True)
            print(result.stdout, end="")
            if "OK_UACCESS_MODEL scope=model" not in result.stdout:
                raise SystemExit(f"{name}: missing completion marker")


if __name__ == "__main__":
    main()
