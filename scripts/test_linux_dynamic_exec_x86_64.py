#!/usr/bin/env python3
"""Run hosted admission tests against explicit shared-musl PIE and loader files."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-elf', type=Path, required=True)
    parser.add_argument('--interpreter-elf', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ok-dynamic-elf-model-') as directory:
        executable = Path(directory) / 'test'
        subprocess.run(['g++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        '-fno-exceptions', '-ffunction-sections', '-fdata-sections',
                        '-DOK_ARCH_TARGET_X86_64', '-I' + str(root / 'include'),
                        str(root / 'tests/host/linux_dynamic_exec_x86_64_tests.cpp'),
                        str(root / 'src/sched/process.cpp'), '-Wl,--gc-sections', '-o', str(executable)],
                       check=True)
        subprocess.run([str(executable), str(args.main_elf.resolve()),
                        str(args.interpreter_elf.resolve())], check=True)


if __name__ == '__main__':
    main()
