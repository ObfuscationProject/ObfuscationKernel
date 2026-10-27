#!/usr/bin/env python3
"""Compile and run the private next-data metadata replay hosted model."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'tests/host/ext4_next_data_replay_tests.cpp'
RECOVERY = ROOT / 'src/fs/ext4_recovery.cpp'
HEADERS = tuple(ROOT / 'include/ok/fs' / name for name in (
    'ext4/txn.hpp', 'ext4/journal.hpp', 'ext4/extent.hpp', 'ext4/layout.hpp'))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not sys.dont_write_bytecode or os.environ.get('PYTHONDONTWRITEBYTECODE') != '1':
        parser.error('run with PYTHONDONTWRITEBYTECODE=1 python3 -B')
    image = args.input.resolve()
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be outside the Kernel source tree')
    if not image.is_file():
        parser.error('input image does not exist')
    compiler = shutil.which('c++')
    if compiler is None:
        parser.error('host C++ compiler is unavailable')
    output.mkdir(parents=True, exist_ok=False)
    executable = output / 'ext4-next-data-replay-tests'
    sanitizer = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    commands = [
        ([compiler, '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror',
          *sanitizer, '-I' + str(ROOT / 'include'), str(SOURCE), str(RECOVERY),
          '-Wl,-l:libext2fs.so.2', '-o', str(executable)], output / 'compile.log'),
        ([str(executable), str(image)], output / 'run.log'),
    ]
    results = []
    for argv, log in commands:
        with log.open('xb') as stream:
            result = subprocess.run(argv, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                    env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1',
                                         'ASAN_OPTIONS': 'detect_leaks=0'}, check=False)
        results.append({'argv': argv, 'returncode': result.returncode,
                        'log': str(log), 'log_sha256': digest(log)})
        if result.returncode:
            break
    receipt = {'schema': 1, 'scope': 'hosted-next-data-metadata-replay-only',
               'status': 'passed' if len(results) == 2 and all(not row['returncode'] for row in results)
               else 'failed', 'source': str(SOURCE), 'source_sha256': digest(SOURCE),
               'recovery_source_sha256': digest(RECOVERY),
               'headers_sha256': {str(path): digest(path) for path in HEADERS},
               'input': str(image), 'input_sha256': digest(image), 'commands': results,
               'qemu': False, 'hardware': False, 'writer_data_flush': False,
               'sanitize': args.sanitize}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    if receipt['status'] != 'passed':
        raise SystemExit('hosted next-data replay test failed; see ' + str(output / 'result.json'))
    print('PASS hosted next-data replay', digest(output / 'result.json'))


if __name__ == '__main__':
    main()
