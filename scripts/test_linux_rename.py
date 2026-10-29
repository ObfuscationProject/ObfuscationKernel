#!/usr/bin/env python3
"""Exercise ten Linux rename ABI profiles through the actual hosted adapter."""
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
    sources = [root / 'tests/host/linux_rename_tests.cpp']
    for subsystem in ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
                      'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
        sources.extend(path for path in sorted((root / 'src' / subsystem).rglob('*.cpp'))
                       if path.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp') and path.parent.name != 'runtime')
    sources.extend(root / path for path in ('src/arch/arch.cpp', 'src/arch/terminal_power.cpp', 'src/arch/x86_64/ops.cpp'))
    with tempfile.TemporaryDirectory(prefix='ok-linux-rename-') as work:
        executable = Path(work) / 'rename-tests'
        command = [*shlex.split(args.cxx), '-std=c++23', '-O2', '-g', '-Wall', '-Wextra',
                   '-Wpedantic', '-Werror', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections',
                   *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
                   '-DOK_ARCH_TARGET_X86_64', '-I', str(root / 'include'),
                   *(str(path) for path in sources), '-o', str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print('PASS ten Linux rename profiles: hosted actual adapter; no guest execution', flush=True)


if __name__ == '__main__':
    main()
