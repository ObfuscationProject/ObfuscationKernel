#!/usr/bin/env python3
"""Compile the next-data replay preflight and completion on all kernel profiles."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_ext4_replay_view import PROFILES, TARGET_FLAGS  # noqa: E402

SOURCES = (
    ROOT / 'tests/host/ext4_next_data_replay_compile.cpp',
    ROOT / 'src/fs/ext4_recovery.cpp',
)
HEADERS = tuple(ROOT / 'include/ok/fs' / name for name in (
    'ext4/txn.hpp', 'ext4/journal.hpp', 'ext4/extent.hpp', 'ext4/layout.hpp'))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_identity(path: Path) -> tuple[int, int, int]:
    data = path.read_bytes()[:20]
    if len(data) != 20 or data[:4] != b'\x7fELF' or data[5] not in (1, 2):
        raise ValueError(f'not an ELF object: {path}')
    order = '<' if data[5] == 1 else '>'
    return data[4], data[5], struct.unpack_from(order + 'H', data, 18)[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not sys.dont_write_bytecode or os.environ.get('PYTHONDONTWRITEBYTECODE') != '1':
        parser.error('run with PYTHONDONTWRITEBYTECODE=1 python3 -B')
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be outside the Kernel source tree')
    toolchains = args.toolchain_root.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands: list[dict[str, object]] = []
    status = 'passed'
    for arch, triple, elf_class, endian, machine in PROFILES:
        compiler = toolchains / triple / 'bin' / (triple + '-g++')
        if not compiler.is_file():
            status = 'failed'
            commands.append({'arch': arch, 'error': f'explicit compiler absent: {compiler}'})
            break
        for source in SOURCES:
            label = 'completion' if source.name.endswith('_compile.cpp') else 'recovery'
            obj = output / f'{arch}-{label}.o'
            log = output / f'{arch}-{label}.log'
            argv = [str(compiler), '-std=c++23', '-O2', '-Wall', '-Wextra',
                    '-Wpedantic', '-Werror', '-ffreestanding', '-fno-exceptions',
                    '-fno-threadsafe-statics', '-fno-use-cxa-atexit', '-frtti',
                    *TARGET_FLAGS[arch], '-DOK_KERNEL_FREESTANDING',
                    '-DOK_ARCH_TARGET_' + arch.upper(), '-I' + str(ROOT / 'include'),
                    '-c', str(source), '-o', str(obj)]
            with log.open('xb') as stream:
                result = subprocess.run(argv, cwd=ROOT, stdout=stream,
                                        stderr=subprocess.STDOUT, check=False)
            record: dict[str, object] = {'arch': arch, 'source': str(source),
                'argv': argv, 'returncode': result.returncode, 'log': str(log),
                'log_sha256': digest(log)}
            if result.returncode == 0:
                actual = elf_identity(obj)
                record['elf_identity'] = actual
                record['object_sha256'] = digest(obj)
                if actual != (elf_class, endian, machine):
                    record['error'] = 'wrong target ELF identity'
            commands.append(record)
            if result.returncode or 'error' in record:
                status = 'failed'
                break
        if status == 'failed':
            break
    receipt = {'schema': 1, 'scope': 'freestanding-next-data-replay-cross-compile',
               'status': status, 'source_sha256': {str(path): digest(path)
               for path in (*SOURCES, *HEADERS)}, 'commands': commands,
               'qemu': False, 'hardware': False}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    if status != 'passed':
        raise SystemExit('next-data replay cross compile failed; see ' + str(output / 'result.json'))
    print('PASS next-data replay cross compile', len(PROFILES), 'profiles',
          digest(output / 'result.json'))


if __name__ == '__main__':
    main()
