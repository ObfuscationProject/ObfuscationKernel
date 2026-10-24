#!/usr/bin/env python3
"""Exercise bounded mounted empty-file creation against a private ext4 image."""

from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys

sys.dont_write_bytecode = True
from test_ext4_mounted import SOURCES


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clean-image', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = args.clean_image.resolve()
    result = {'scope': 'hosted-mounted-empty-create', 'status': 'running',
              'clean_image': str(image), 'clean_image_sha256': digest(image),
              'sanitized': args.sanitize, 'native_execution': False,
              'hardware_power_loss': False, 'commands': []}
    relevant = ['scripts/test_ext4_mounted_create.py', 'tests/host/ext4_mounted_create_tests.cpp',
                'include/ok/fs/ext4_mounted_metadata.hpp', 'include/ok/fs/ext4_owned_transaction.hpp',
                'include/ok/fs/ext4_create_transaction.hpp', 'include/ok/fs/ext4_create_plan.hpp',
                'include/ok/fs/ext4_completion.hpp', 'include/ok/fs/ext4_replay_view.hpp', *SOURCES]
    result['source_sha256'] = {name: digest(root / name) for name in relevant}

    def save() -> None:
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')

    def run(label: str, command: list[str]) -> str:
        record = {'label': label, 'argv': [str(item) for item in command]}
        result['commands'].append(record)
        save()
        completed = subprocess.run(record['argv'], text=True, capture_output=True)
        text = completed.stdout + completed.stderr
        (output / (label + '.log')).write_text(text)
        record['returncode'] = completed.returncode
        save()
        if completed.returncode != 0:
            raise RuntimeError(f'{label} failed; inspect {label}.log')
        return text

    try:
        flags = ['g++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                 '-fno-access-control', '-DOK_ARCH_TARGET_X86_64', '-I' + str(root / 'include')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        source = root / 'tests/host/ext4_mounted_create_tests.cpp'
        binary = output / 'mounted-create-tests'
        run('compile', [*flags, str(source), *(str(root / path) for path in SOURCES),
                        '-Wl,-l:libext2fs.so.2', '-pthread', '-o', str(binary)])
        generated = output / 'created.img'
        report = run('hosted', [str(binary), str(image), str(generated)])
        if 'PASS mounted empty create:' not in report:
            raise RuntimeError('hosted result lacks pass marker')
        run('readonly-fsck', ['e2fsck', '-fn', str(generated)])
        for name in ('hello', 'posix'):
            details = run('debugfs-' + name, ['debugfs', '-R', 'stat /' + name, str(generated)])
            if 'Inode:' not in details or 'Size: 0' not in details:
                raise RuntimeError(f'debugfs did not verify empty {name} inode')
        if digest(image) != result['clean_image_sha256']:
            raise RuntimeError('input image changed')
        result['created_image_sha256'] = digest(generated)
        result['status'] = 'passed'
        print(report, end='')
    except Exception as error:
        result.update(status='failed', error=str(error))
        print(f'FAIL mounted create: {error}', file=sys.stderr)
    finally:
        save()
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
