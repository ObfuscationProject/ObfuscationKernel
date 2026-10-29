#!/usr/bin/env python3
"""Build the clean 32 MiB journalled ext4 image the hosted ext4 runners take as --clean-image.

The layout matches ObfuscationOS tools/jbd2/create_linux_fixture.py baseline-clean.img
(1 KiB blocks, 256-byte inodes, has_journal/extents/filetype/metadata_csum, a 4 MiB
journal and a 4 KiB /seed file). Instead of a first Linux mount, debugfs opens and
closes the journal with CRC32C v3 checksums, which is the JBD2 profile the mounted
writer admits. e2fsck must find the result clean.
"""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import tempfile

UUID = 'f2026092-0000-4000-8000-000000000001'


def tool(name):
    found = shutil.which(name) or shutil.which(name, path='/usr/sbin:/sbin')
    if not found:
        raise SystemExit(f'{name} (e2fsprogs) is required')
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New image path')
    args = parser.parse_args()
    image = args.output.resolve()
    if image.exists():
        raise SystemExit(f'{image} exists')
    image.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ok-ext4-fixture-') as directory:
        work = Path(directory)
        staged = work / 'clean.img'
        with staged.open('xb') as stream:
            stream.truncate(32 * 1024 * 1024)
        subprocess.run([tool('mke2fs'), '-q', '-F', '-t', 'ext4', '-b', '1024', '-I', '256', '-O',
                        'none,has_journal,extents,filetype,metadata_csum', '-J', 'size=4', '-E',
                        'lazy_itable_init=0,lazy_journal_init=0', '-U', UUID, str(staged)], check=True)
        (work / 'seed.bin').write_bytes(bytes((index * 17 + 29) & 255 for index in range(4096)))
        (work / 'seed.debugfs').write_text('write seed.bin /seed\nset_inode_field /seed mode 0100644\n'
                                           'set_inode_field /seed uid 0\nset_inode_field /seed gid 0\n'
                                           'jo -c -v 3\njc\n')
        subprocess.run([tool('debugfs'), '-w', '-f', 'seed.debugfs', str(staged)], cwd=work, check=True,
                       capture_output=True)
        subprocess.run([tool('e2fsck'), '-fn', str(staged)], check=True, capture_output=True)
        shutil.copyfile(staged, image)
    print(image, hashlib.sha256(image.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
