#!/usr/bin/env python3
"""Model test: kernel-section bodies (ok/user/linux_file_io.hpp) equal the kernel-lock path, and section
admission declines in order with no effect, on all ten Linux profiles with a hosted mapping/copy backend.

Built with OK_LOCK_DEBUG so admission checks each decline against its digest (A5). Model coverage only:
no CPU leaves the kernel lock, no section is pinned and no guest executes.
"""
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
    sources = [root/'tests/host/linux_file_io_equivalence_tests.cpp']
    for directory in ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
                      'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
        sources.extend(path for path in sorted((root/'src'/directory).rglob('*.cpp'))
                       if path.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp') and path.parent.name != 'runtime')
    sources.extend(root/path for path in ('src/arch/arch.cpp', 'src/arch/terminal_power.cpp', 'src/arch/x86_64/ops.cpp'))
    with tempfile.TemporaryDirectory(prefix='ok-linux-file-io-') as directory:
        executable = Path(directory)/'file-io-equivalence-tests'
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wpedantic', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
                        '-DOK_ARCH_TARGET_X86_64', '-DOK_LOCK_DEBUG', '-I', str(root/'include'),
                        *(str(path) for path in sources), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print('PASS all ten Linux file I/O equivalence profiles: hosted section admission and shared bodies; no section pinned, no guest execution', flush=True)


if __name__ == '__main__':
    main()
