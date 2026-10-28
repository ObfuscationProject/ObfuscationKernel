#!/usr/bin/env python3
"""Cross-compile the bounded Linux bootstrap adapter; never execute guest code."""
import argparse
import subprocess
import tempfile
from pathlib import Path
from test_linux_exec import PROFILES, TARGET_FLAGS

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--toolchain-root', type=Path, default=root / 'toolchains',
                    help='Directory holding the ten <triple>/bin toolchains')
toolchains = parser.parse_args().toolchain_root.resolve()
with tempfile.TemporaryDirectory(prefix='ok-linux-bootstrap-') as directory:
    for arch, triple, elfclass, endian, machine in PROFILES:
        compiler = toolchains / triple / 'bin' / (triple + '-g++')
        subprocess.run([str(compiler), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-ffreestanding', '-fno-exceptions', '-fno-threadsafe-statics',
                        *TARGET_FLAGS[arch], '-DOK_ARCH_TARGET_' + arch.upper(), '-I' + str(root / 'include'),
                        '-c', str(root / 'tests/host/linux_bootstrap_compile.cpp'),
                        '-o', str(Path(directory) / (arch + '.o'))], check=True)
        print(f'PASS {arch}: Linux bootstrap adapter target compilation only', flush=True)
