#!/usr/bin/env python3
"""Exercise all ten Linux seek ABI profiles with a hosted mapping/copy backend."""
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
    sources = [root/'tests/host/linux_seek_tests.cpp']
    for directory in ('core', 'apps', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
                      'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
        sources.extend(path for path in sorted((root/'src'/directory).rglob('*.cpp'))
                       if path.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp'))
    sources.extend(root/path for path in ('src/arch/arch.cpp', 'src/arch/x86_64/ops.cpp'))
    with tempfile.TemporaryDirectory(prefix='ok-linux-seek-') as directory:
        executable = Path(directory)/'seek-tests'
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wpedantic', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
                        '-DOK_ARCH_TARGET_X86_64', '-I', str(root/'include'),
                        *(str(path) for path in sources), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print('PASS all ten Linux seek profiles: hosted adapter/service models; no guest execution', flush=True)


if __name__ == '__main__':
    main()
