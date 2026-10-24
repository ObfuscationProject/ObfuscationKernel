#!/usr/bin/env python3
"""Run the freestanding JBD2 codec and replay PLAN as hosted model tests only."""
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
    subprocess.run([sys.executable, str(root / 'scripts/generate_jbd2_fixture.py'), '--check'], check=True)
    with tempfile.TemporaryDirectory(prefix='ok-jbd2-model-') as work:
        binary = Path(work) / 'jbd2-tests'
        command = [*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                   '-fno-exceptions', '-I' + str(root / 'include'), str(root / 'tests/host/jbd2_tests.cpp'),
                   '-Wl,-l:libext2fs.so.2', '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(root / 'tests/fixtures/jbd2_v3.bin')], check=True)


if __name__ == '__main__':
    main()
