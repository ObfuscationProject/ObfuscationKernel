#!/usr/bin/env python3
"""Run software realtime coordinator/helper models; no hardware or user execution."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ok-realtime-clock-') as directory:
        output = Path(directory)/'models'
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O1' if args.sanitize else '-O2',
                        '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
                        '-I', str(root/'include'), str(root/'tests/host/realtime_clock_tests.cpp'),
                        '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True)
    print('PASS hosted realtime coordinator and validation models; no hardware execution')


if __name__ == '__main__':
    main()
