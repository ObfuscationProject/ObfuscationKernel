#!/usr/bin/env python3
"""Hosted block-file integration: mutate ext4, reopen it, and run offline fsck."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def debugfs(disk, command):
    return subprocess.run(['debugfs', '-w', '-R', command, str(disk)], check=True,
                          text=True, capture_output=True).stdout


def inode_offset(disk, name):
    report = debugfs(disk, f'stat /{name}')
    match = re.search(r'Inode:\s+(\d+)\s+Type:\s+symlink', report)
    assert match, report
    number = int(match[1])
    raw = disk.read_bytes()
    u32 = lambda at: struct.unpack_from('<I', raw, at)[0]
    block_size = 1024 << u32(1048)
    group, index = divmod(number - 1, u32(1064))
    table = u32((u32(1044) + 1) * block_size + group * 32 + 8)
    return table * block_size + index * struct.unpack_from('<H', raw, 1112)[0]


def upstream_symlinks(disk, block_size):
    for name, length, fill in [('upstream59', 59, 'u'), ('upstream60', 60, 'u'),
                               ('upstreammax', block_size - 1, 'u'), ('legacy60', 60, 'l'),
                               ('legacy-drop', 60, 'l')]:
        debugfs(disk, f'symlink /{name} {fill * length}')
    # An old direct-block symlink uses the same allocated data block. This
    # fixture changes only its inode representation, then offline fsck checks it.
    for name in ('legacy60', 'legacy-drop'):
        offset = inode_offset(disk, name)
        raw = bytearray(disk.read_bytes())
        physical = struct.unpack_from('<I', raw, offset + 60)[0]
        assert struct.unpack_from('<I', raw, offset + 32)[0] == 0x80000
        raw[offset + 40:offset + 100] = bytes(60)
        struct.pack_into('<I', raw, offset + 32, 0)
        struct.pack_into('<I', raw, offset + 40, physical)
        disk.write_bytes(raw)
    subprocess.run(['e2fsck', '-fn', str(disk)], check=True)


def verify_symlink_format(disk, block_size):
    for length in (1, 59, 60, 255, block_size - 1):
        offset = inode_offset(disk, f's{length}')
        raw = disk.read_bytes()
        u32 = lambda at: struct.unpack_from('<I', raw, at)[0]
        assert u32(offset + 4) == length
        assert u32(offset + 28) == (0 if length < 60 else block_size // 512)
        assert u32(offset + 32) == (0 if length < 60 else 0x80000)
        payload = offset + 40 if length < 60 else u32(offset + 60) * block_size
        assert raw[payload:payload + length + 1] == b's' * length + b'\0'


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
                        str(root/'tests/host/ext4_realtime_model.cpp'),
                        '-o', str(app)], check=True)
        faults = tmp/'ext4-failure-test'
        subprocess.run([args.cxx, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DOK_ARCH_TARGET_X86_64', '-I'+str(root/'include'),
                        str(root/'tests/host/ext4_failure_tests.cpp'), str(root/'src/fs/ext4_fs.cpp'),
                        str(root/'tests/host/ext4_realtime_model.cpp'),
                        '-o', str(faults)], check=True)
        for size in (1024, 2048, 4096):
            disk = tmp/f'ext4-{size}.img'
            subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-b', str(size), '-I', '256',
                            '-O', 'none,extent,filetype,sparse_super,large_file',
                            '-E', 'lazy_itable_init=0', '-m', '0', str(disk), '16M'], check=True)
            upstream_symlinks(disk, size)
            subprocess.run([str(app), str(disk), 'write'], check=True)
            subprocess.run([str(app), str(disk), 'verify'], check=True)
            verify_symlink_format(disk, size)
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
