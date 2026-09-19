#!/usr/bin/env python3
"""Hosted block-file integration: mutate ext4, reopen it, and run offline fsck."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ok-ext4-') as tmp:
        tmp = Path(tmp)
        app = tmp/'ext4-test'
        subprocess.run([args.cxx, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DOK_ARCH_TARGET_X86_64', '-I'+str(root/'include'),
                        str(root/'tests/host/ext4_fs_tests.cpp'), str(root/'src/fs/ext4_fs.cpp'),
                        '-o', str(app)], check=True)
        faults = tmp/'ext4-failure-test'
        subprocess.run([args.cxx, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DOK_ARCH_TARGET_X86_64', '-I'+str(root/'include'),
                        str(root/'tests/host/ext4_failure_tests.cpp'), str(root/'src/fs/ext4_fs.cpp'),
                        '-o', str(faults)], check=True)
        for size in (1024, 2048, 4096):
            disk = tmp/f'ext4-{size}.img'
            subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-b', str(size), '-I', '256',
                            '-O', 'none,extent,filetype,sparse_super,large_file',
                            '-E', 'lazy_itable_init=0', '-m', '0', str(disk), '16M'], check=True)
            subprocess.run([str(app), str(disk), 'write'], check=True)
            subprocess.run([str(app), str(disk), 'verify'], check=True)
            subprocess.run([str(faults), str(disk)], check=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            subprocess.run([str(app), str(disk), 'readonly'], check=True)
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            subprocess.run(['e2fsck', '-fn', str(disk)], check=True)
            unclean = bytearray(disk.read_bytes())
            unclean[1024+58:1024+60] = b'\0\0'
            disk.write_bytes(unclean)
            subprocess.run([str(app), str(disk), 'reject'], check=True)
            assert disk.read_bytes() == unclean
            print(f'PASS hosted ext4 {size}-byte blocks: mutation, reopen and offline e2fsck', flush=True)
        unsupported = tmp/'default-ext4.img'
        subprocess.run(['mke2fs', '-q', '-t', 'ext4', str(unsupported), '16M'], check=True)
        before = hashlib.sha256(unsupported.read_bytes()).digest()
        subprocess.run([str(app), str(unsupported), 'reject'], check=True)
        assert hashlib.sha256(unsupported.read_bytes()).digest() == before


if __name__ == '__main__':
    main()
