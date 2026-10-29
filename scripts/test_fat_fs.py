#!/usr/bin/env python3
"""Build and exercise the bounded FAT VFS against independent format/fsck tools.

Hosted block-device tests only: no guest, hardware or power-loss qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--tools', required=True, type=Path, help='Explicit directory with mkfs.fat, mkfs.exfat and fsck tools')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source'
    root = Path(__file__).resolve().parents[1]
    names = ['include/ok/fs/fat.hpp', 'include/ok/fs/fat_fs.hpp', 'src/fs/fat.cpp', 'src/fs/fat_fs.cpp',
             'tests/host/fat_fs_tests.cpp', 'scripts/test_fat_fs.py', 'src/fs/vfs.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp', 'src/arch/x86_64/ops.cpp', 'src/arch/ops_private.hpp']
    shutil.copytree(root / 'include', source / 'include')
    for name in names:
        destination = source / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, destination)
    project = ['set_project("fat-vfs")', 'set_languages("c++23")', 'target("fat-vfs")',
               'set_kind("binary")', 'set_targetdir("artifacts")', 'add_defines("OK_ARCH_TARGET_X86_64")',
               'add_includedirs("include")',
               'add_files("tests/host/fat_fs_tests.cpp", "src/fs/fat.cpp", "src/fs/fat_fs.cpp", "src/fs/vfs.cpp", "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/terminal_power.cpp", "src/arch/x86_64/ops.cpp")',
               'add_cxxflags("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-fstack-usage", {force=true})']
    project += ['add_ldflags("-Wl,--gc-sections", {force=true})']
    if args.sanitize:
        project += ['add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force=true})',
                    'add_ldflags("-fsanitize=address,undefined", {force=true})']
    (source / 'xmake.lua').write_text('\n'.join(project + ['target_end()']) + '\n')
    tools = args.tools.resolve()
    identities = {str(tools / name): digest(tools / name) for name in ['mkfs.fat', 'mkfs.exfat', 'fsck.fat', 'fsck.exfat']}
    result = {'scope': 'hosted-block-device; no guest/hardware/power-loss qualification', 'status': 'running',
              'source_sha256': {name: digest(source / name) for name in names}, 'tools_sha256': identities,
              'sanitize': args.sanitize, 'leak_detection': False, 'commands': []}
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')

    def run(label, argv):
        argv = list(map(str, argv))
        with (output / (label + '.log')).open('x') as log:
            done = subprocess.run(argv, cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT)
        result['commands'].append({'label': label, 'argv': argv, 'exit_code': done.returncode,
                                   'log_sha256': digest(output / (label + '.log'))})
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        if done.returncode:
            raise RuntimeError(label)

    def image(name, data):
        target = output / name
        target.write_bytes(data)
        return target

    try:
        run('configure', ['xmake', 'f', '-c', '-y', '--ccache=n'])
        run('build', ['xmake', 'build', '-j2'])
        binary = source / 'artifacts/fat-vfs'
        for kind in ['fat32', 'exfat']:
            disk = output / (kind + '.img')
            with disk.open('xb') as file:
                file.truncate(64 * 1024 * 1024)
            run(kind + '-mkfs', [tools / ('mkfs.fat' if kind == 'fat32' else 'mkfs.exfat'),
                                 *(['-F', '32'] if kind == 'fat32' else []), disk])
            pristine = disk.read_bytes()
            run(kind + '-write', [binary, disk, 'write'])
            run(kind + '-verify', [binary, disk, 'verify'])
            run(kind + '-fsck', [tools / ('fsck.fat' if kind == 'fat32' else 'fsck.exfat'), '-n', disk])
            valid = disk.read_bytes()
            # Admission must reject corruption before the first device write.
            bad = bytearray(pristine)
            bad[510] ^= 1
            run(kind + '-signature', [binary, image(kind + '-signature.img', bad), 'reject'])
            bad = bytearray(pristine)
            if kind == 'fat32':
                fat = struct.unpack_from('<H', bad, 14)[0] * 512
                fat_size = struct.unpack_from('<I', bad, 36)[0] * 512
                root_cluster = struct.unpack_from('<I', bad, 44)[0]
                for copy in range(bad[16]):
                    struct.pack_into('<I', bad, fat + copy * fat_size + root_cluster * 4, root_cluster)
            else:
                fat = struct.unpack_from('<I', bad, 80)[0] * 512
                root_cluster = struct.unpack_from('<I', bad, 96)[0]
                struct.pack_into('<I', bad, fat + root_cluster * 4, root_cluster)
            run(kind + '-cycle', [binary, image(kind + '-cycle.img', bad), 'reject'])
            bad = bytearray(pristine)
            if kind == 'fat32':
                # Clean-volume and mirrored FAT admission must precede mutation.
                for copy in range(bad[16]):
                    struct.pack_into('<I', bad, fat + copy * fat_size + 4, 0x07ffffff)
                run('fat32-dirty', [binary, image('fat32-dirty.img', bad), 'reject'])
                bad = bytearray(pristine)
                struct.pack_into('<I', bad, fat + fat_size + root_cluster * 4, 0)
                run('fat32-mirror', [binary, image('fat32-mirror.img', bad), 'reject'])
                bad = bytearray(valid)
                cluster_bytes = bad[13] * 512
                heap = fat + bad[16] * fat_size
                root_at = heap + (root_cluster - 2) * cluster_bytes
                folder = next(at for at in range(root_at, root_at + cluster_bytes, 32)
                              if bad[at] not in (0, 0xe5) and bad[at + 11] == 0x10)
                folder_cluster = struct.unpack_from('<H', bad, folder + 26)[0] | (struct.unpack_from('<H', bad, folder + 20)[0] << 16)
                folder_at = heap + (folder_cluster - 2) * cluster_bytes
                file_at = next(at for at in range(folder_at, folder_at + cluster_bytes, 32)
                               if bad[at] not in (0, 0xe5) and bad[at + 11] == 0x20)
                struct.pack_into('<H', bad, file_at + 20, folder_cluster >> 16)
                struct.pack_into('<H', bad, file_at + 26, folder_cluster & 0xffff)
                struct.pack_into('<I', bad, file_at + 28, 512)
                run('fat32-crosslink', [binary, image('fat32-crosslink.img', bad), 'reject'])
            else:
                # A used root cluster must remain marked allocated in the bitmap.
                heap = struct.unpack_from('<I', bad, 88)[0] * 512
                cluster_bytes = 512 << bad[109]
                root_at = heap + (root_cluster - 2) * cluster_bytes
                bitmap_at = next(at for at in range(root_at, root_at + cluster_bytes, 32) if bad[at] == 0x81)
                bitmap_cluster = struct.unpack_from('<I', bad, bitmap_at + 20)[0]
                bitmap = heap + (bitmap_cluster - 2) * cluster_bytes
                bad[bitmap + (root_cluster - 2) // 8] &= ~(1 << ((root_cluster - 2) % 8))
                run('exfat-bitmap', [binary, image('exfat-bitmap.img', bad), 'reject'])
                bad = bytearray(valid)
                folder = next(at for at in range(root_at, root_at + cluster_bytes, 32) if bad[at] == 0x85)
                folder_cluster = struct.unpack_from('<I', bad, folder + 52)[0]
                file_at = heap + (folder_cluster - 2) * cluster_bytes
                count = bad[file_at + 1] + 1
                struct.pack_into('<I', bad, file_at + 52, folder_cluster)
                struct.pack_into('<Q', bad, file_at + 40, cluster_bytes)
                struct.pack_into('<Q', bad, file_at + 56, cluster_bytes)
                checksum = 0
                for i, byte in enumerate(bad[file_at:file_at + count * 32]):
                    if i not in (2, 3):
                        checksum = (((checksum & 1) << 15) + (checksum >> 1) + byte) & 0xffff
                struct.pack_into('<H', bad, file_at + 2, checksum)
                run('exfat-crosslink', [binary, image('exfat-crosslink.img', bad), 'reject'])
            for failure in (1, 2, 3, 4, 5, 6, 8, 10):
                run(kind + '-fault-' + str(failure), [binary, image(kind + '-fault-' + str(failure) + '.img', pristine), 'fault', failure])
            if kind == 'exfat':
                bad = bytearray(pristine)
                bad[120] ^= 1
                run('exfat-boot-checksum', [binary, image('exfat-boot-checksum.img', bad), 'reject'])
                raw = bytearray(valid)
                heap = struct.unpack_from('<I', raw, 88)[0] * 512
                cluster_bytes = 512 << raw[109]
                root_at = heap + (struct.unpack_from('<I', raw, 96)[0] - 2) * cluster_bytes
                folder = next(at for at in range(root_at, root_at + cluster_bytes, 32) if raw[at] == 0x85)
                folder_cluster = struct.unpack_from('<I', raw, folder + 52)[0]
                file_at = heap + (folder_cluster - 2) * cluster_bytes
                assert raw[file_at] == 0x85
                count = raw[file_at + 1] + 1
                first = struct.unpack_from('<I', raw, file_at + 52)[0]
                length = struct.unpack_from('<Q', raw, file_at + 56)[0]
                clusters = (length + cluster_bytes - 1) // cluster_bytes
                for i in range(clusters):
                    value = struct.unpack_from('<I', raw, fat + (first + i) * 4)[0]
                    assert value == first + i + 1 or (i == clusters - 1 and value >= 0xfffffff8)
                    struct.pack_into('<I', raw, fat + (first + i) * 4, 0)
                raw[file_at + 33] = 3
                checksum = 0
                for i, byte in enumerate(raw[file_at:file_at + count * 32]):
                    if i not in (2, 3):
                        checksum = (((checksum & 1) << 15) + (checksum >> 1) + byte) & 0xffff
                struct.pack_into('<H', raw, file_at + 2, checksum)
                contiguous = image('exfat-contiguous.img', raw)
                run('exfat-contiguous-reference', [tools / 'fsck.exfat', '-n', contiguous])
                run('exfat-contiguous-verify', [binary, contiguous, 'verify'])
                run('exfat-contiguous-extend', [binary, contiguous, 'contiguous'])
                run('exfat-contiguous-final', [binary, contiguous, 'verify'])
                run('exfat-contiguous-fsck', [tools / 'fsck.exfat', '-n', contiguous])
        result['status'] = 'passed'
    except BaseException as error:
        result.update(status='failed', error=repr(error))
        raise
    finally:
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('FAT VFS hosted interoperability, malformed media and failure controls passed')


if __name__ == '__main__':
    main()
