#!/usr/bin/env python3
"""Run all ten Linux userspace ABI codec specializations as hosted model tests."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    subprocess.run([sys.executable, str(root / "tests/host/linux_abi_generator_tests.py")], check=True)
    with tempfile.TemporaryDirectory(prefix='ok-linux-abi-model-') as work:
        binary = Path(work) / 'linux-user-abi-tests'
        command = [*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                   '-fno-exceptions', '-I' + str(root / 'include'), str(root / 'tests/host/linux_user_abi_tests.cpp'), '-o', str(binary)]
        if args.sanitize:
            command[2:2] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
