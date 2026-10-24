#!/usr/bin/env python3
"""Exercise the shared deadline state machine without hardware execution."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ok-deadline-') as directory:
    executable = Path(directory) / 'deadline'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    *flags, '-I' + str(root / 'include'), str(root / 'tests/host/deadline_tests.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
