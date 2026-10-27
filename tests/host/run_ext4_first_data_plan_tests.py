#!/usr/bin/env python3
"""Compile and run the pure first-data allocation image model only."""
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
SOURCE = ROOT / 'tests/host/ext4_first_data_plan_tests.cpp'
HEADER = ROOT / 'include/ok/fs/ext4/extent.hpp'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not sys.dont_write_bytecode or os.environ.get('PYTHONDONTWRITEBYTECODE') != '1':
        parser.error('run with PYTHONDONTWRITEBYTECODE=1 python3 -B')
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be outside the Kernel source tree')
    compiler = shutil.which('c++')
    if compiler is None:
        parser.error('host C++ compiler is unavailable')
    output.mkdir(parents=True, exist_ok=False)
    executable = output / 'ext4-first-data-plan-tests'
    commands = [
        ([compiler, '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror',
          '-I' + str(ROOT / 'include'), str(SOURCE), '-o', str(executable)], output / 'compile.log'),
        ([str(executable)], output / 'run.log'),
    ]
    results = []
    for argv, log in commands:
        with log.open('xb') as stream:
            result = subprocess.run(argv, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                    env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'}, check=False)
        results.append({'argv': argv, 'returncode': result.returncode,
                        'log': str(log), 'log_sha256': digest(log)})
        if result.returncode:
            break
    receipt = {'schema': 1, 'scope': 'hosted-ext4-first-data-plan-only',
               'status': 'passed' if len(results) == 2 and all(not row['returncode'] for row in results)
               else 'failed', 'source': str(SOURCE), 'source_sha256': digest(SOURCE),
               'header': str(HEADER), 'header_sha256': digest(HEADER),
               'commands': results, 'qemu': False, 'hardware': False, 'disk_writes': False}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    if receipt['status'] != 'passed':
        raise SystemExit('hosted first-data plan test failed; see ' + str(output / 'result.json'))
    print('PASS hosted first-data plan', digest(output / 'result.json'))


if __name__ == '__main__':
    main()
