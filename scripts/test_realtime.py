#!/usr/bin/env python3
"""Check Gregorian/anchor/register mechanisms against independent date/integer oracles."""
import argparse
import datetime
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    epoch = datetime.datetime(1970, 1, 1)
    values = [datetime.datetime(year, month, day, 12, 34, 56)
              for year in range(1, 10000) for month, day in ((1, 1), (3, 1), (12, 31))]
    rng = random.Random(1737)
    for _ in range(10000):
        value = datetime.datetime.fromordinal(rng.randrange(1, datetime.datetime(9999, 12, 31).toordinal() + 1))
        values.append(value.replace(hour=rng.randrange(24), minute=rng.randrange(60), second=rng.randrange(60)))
    vectors = ''.join(f'{v.year} {v.month} {v.day} {v.hour} {v.minute} {v.second} {(v - epoch).days * 86400 + (v - epoch).seconds}\n' for v in values)
    with tempfile.TemporaryDirectory(prefix='ok-realtime-') as directory:
        binary = Path(directory) / 'tests'
        for optimization in ('-O0', '-O2'):
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if args.sanitize else []
            command = [*shlex.split(args.cxx), '-std=c++23', optimization, '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                       '-fno-exceptions', *flags, '-I', str(root/'include'), str(root/'tests/host/realtime_tests.cpp'), '-o', str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], input=vectors, text=True, check=True)
            print('PASS', optimization, 'sanitized' if args.sanitize else 'normal', flush=True)

if __name__ == '__main__':
    main()
