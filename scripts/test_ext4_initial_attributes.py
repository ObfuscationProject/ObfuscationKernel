#!/usr/bin/env python3
"""Hosted ext4 initial-attribute publication, capacity rollback and I/O faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_created(path):
    data = path.read_bytes()
    u16 = lambda at: struct.unpack_from('<H', data, at)[0]
    u32 = lambda at: struct.unpack_from('<I', data, at)[0]
    size, per, stride = 1024 << u32(1048), u32(1064), u16(1112)

    def inode(number):
        group, index = divmod(number - 1, per)
        table = u32((u32(1044) + 1) * size + group * 32 + 8)
        return table * size + index * stride

    def children(number):
        node, output = inode(number), {}
        assert u16(node + 40) == 0xf30a and u16(node + 46) == 0
        for index in range(u16(node + 42)):
            first, count = u32(node + 60 + index * 12), u16(node + 56 + index * 12)
            for block in range(first, first + count):
                at, end = block * size, (block + 1) * size
                while at < end:
                    length, namesize = u16(at + 4), data[at + 6]
                    assert length >= 8 and length % 4 == 0 and at + length <= end and namesize <= length - 8
                    if u32(at):
                        name = data[at + 8:at + 8 + namesize].decode()
                        assert name not in output
                        output[name] = (u32(at), data[at + 7])
                    at += length
        return output

    root = children(2)
    number, dtype = root['initial-dir']
    assert dtype == 2
    nested = children(number)
    assert nested['.'] == (number, 2) and nested['..'] == (2, 2)
    file, dtype = nested['full-owner']
    assert dtype == 1 and file != number
    for child, mode, uid, gid, links in [(number, 0o43750, 0x12345678, 0x87654321, 2),
                                        (file, 0o107777, 0xffffffff, 0xfedcba98, 1)]:
        node = inode(child)
        assert u16(node) == mode and u16(node + 26) == links
        assert u16(node + 2) | u16(node + 120) << 16 == uid
        assert u16(node + 24) | u16(node + 122) << 16 == gid
    assert u16(inode(2) + 26) == 4  # Root, lost+found and the one new directory.
    return {'directory_inode': number, 'regular_inode': file, 'raw_owner_mode_dot_link_checks': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'hosted-ext4-initial-attributes', 'status': 'running',
              'sanitize': args.sanitize, 'native_execution': False, 'physical_durability': False,
              'fault_scope': 'hosted rejected, partial and completed transport failures',
              'synthetic_capacity_images_fsck_qualified': False, 'commands': [], 'cases': []}

    def save():
        (output/'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command, label, env=None):
        result = subprocess.run([str(item) for item in command], capture_output=True, text=True, env=env)
        (output/(label+'.log')).write_text(result.stdout+result.stderr)
        report['commands'].append({'argv': [str(item) for item in command], 'label': label,
                                   'returncode': result.returncode})
        save()
        if result.returncode:
            raise RuntimeError(label+' failed; see '+str(output/(label+'.log')))
        return result.stdout

    try:
        snapshot = output/'sources'
        shutil.copytree(root/'include', snapshot/'include')
        for name in ['src/fs/ext4_fs.cpp', 'tests/host/ext4_initial_attributes.cpp',
                     'tests/host/ext4_realtime_model.cpp', 'tests/host/ext4_realtime_model.hpp',
                     'scripts/test_ext4_initial_attributes.py']:
            path = snapshot/name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root/name, path)
        report['source_sha256'] = {str(p.relative_to(snapshot)): sha(p) for p in sorted(snapshot.rglob('*')) if p.is_file()}
        flags = ['-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-DOK_ARCH_TARGET_X86_64', '-I'+str(snapshot/'include')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        binary = output/'ext4-initial-attributes'
        run([args.cxx, *flags, snapshot/'tests/host/ext4_initial_attributes.cpp',
             snapshot/'src/fs/ext4_fs.cpp', snapshot/'tests/host/ext4_realtime_model.cpp',
             '-o', binary], 'compile')
        environment = os.environ.copy()
        if args.sanitize:
            environment['ASAN_OPTIONS'] = 'detect_leaks=0'
            report['asan_options'] = environment['ASAN_OPTIONS']
        for size in (1024, 2048, 4096):
            for stride in (128, 256):
                label = f'block{size}-inode{stride}'
                folder = output/label
                folder.mkdir()
                disk = folder/'input.img'
                run(['mke2fs', '-q', '-t', 'ext4', '-b', size, '-I', stride,
                     '-O', 'none,extent,filetype,sparse_super,large_file',
                     '-E', 'lazy_itable_init=0', '-m', '0', disk, '16M'], label+'-mkfs')
                log = run([binary, disk, folder], label+'-test', environment)
                assert log.count('PASS ext4 initial attributes ') == 1
                for name in ('created', 'node-cache', 'extent-rollback'):
                    run(['e2fsck', '-fn', folder/(name+'.img')], label+'-'+name+'-fsck')
                facts = verify_created(folder/'created.img')
                report['cases'].append({'block_size': size, 'inode_size': stride, 'status': 'passed', **facts})
                print('PASS', label, flush=True)
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['artifact_sha256'] = {str(p.relative_to(output)): sha(p) for p in sorted(output.rglob('*'))
                                     if p.is_file() and p.name != 'result.json'}
        save()


if __name__ == '__main__':
    main()
