#!/usr/bin/env python3
"""Run hosted E820 parser checks; these do not qualify physical RAM allocation."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ok-e820-memory-') as directory:
        output = Path(directory) / 'e820-memory'
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra',
            '-Wpedantic', '-Werror', '-fno-exceptions', *flags,
            f'-I{root / "include"}', str(root / 'tests/host/e820_memory_tests.cpp'),
            '-o', str(output),
        ], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
