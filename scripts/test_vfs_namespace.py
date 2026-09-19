#!/usr/bin/env python3
"""Host RAM filesystem namespace and retained-inode lifecycle checks."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = ('tests/host/vfs_namespace_tests.cpp', 'src/fs/vfs.cpp', 'src/smp/smp.cpp',
               'src/arch/arch.cpp', 'src/arch/x86_64/ops.cpp')
    with tempfile.TemporaryDirectory(prefix='ok-vfs-namespace-') as tmp:
        app = Path(tmp)/'namespace-test'
        subprocess.run([args.cxx, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-DOK_ARCH_TARGET_X86_64', '-I'+str(root/'include'),
                        *(str(root/source) for source in sources), '-o', str(app)], check=True)
        subprocess.run([str(app)], check=True)


if __name__ == '__main__':
    main()
