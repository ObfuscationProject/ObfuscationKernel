#!/usr/bin/env python3
"""Qualify the private bounded next-block mounted path on an exact source copy."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from test_ext4_mounted import SOURCES
from test_ext4_owned import PROFILES, TARGET_FLAGS


ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--empty-created-image', type=Path, required=True)
    parser.add_argument('--input-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain-root', type=Path)
    parser.add_argument('--cross', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not (__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1'):
        parser.error('run with PYTHONDONTWRITEBYTECODE=1 python3 -B')
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be outside Kernel source tree')
    output.mkdir(parents=True, exist_ok=False)
    report = {'schema': 1, 'scope': 'hosted private next-data mounted path and ten-profile compile',
              'status': 'running', 'source_sdk_abi': 48, 'qemu': False, 'physical': False,
              'full_m0_m12': False, 'promoted': False, 'sanitize': args.sanitize,
              'cross': args.cross, 'asan_options': os.environ.get('ASAN_OPTIONS', ''),
              'ubsan_options': os.environ.get('UBSAN_OPTIONS', ''), 'commands': []}

    def save() -> None:
        (output / 'result-v1.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')

    def run(argv: list[object], label: str, allowed: tuple[int, ...] = (0,)) -> str:
        command = [str(x) for x in argv]
        row = {'argv': command, 'label': label, 'status': 'running'}
        report['commands'].append(row)
        save()
        start = time.monotonic()
        result = subprocess.run(command, cwd=output, capture_output=True, text=True, timeout=300)
        log = output / (label + '.log')
        log.write_text(result.stdout + result.stderr)
        row.update(returncode=result.returncode,
                   status='passed' if result.returncode in allowed else 'failed',
                   seconds=time.monotonic() - start, log=str(log), log_sha256=digest(log))
        save()
        if result.returncode not in allowed:
            raise RuntimeError(label + ' failed; inspect retained log')
        return result.stdout + result.stderr

    try:
        original = args.empty_created_image.resolve()
        if digest(original) != args.input_sha256:
            raise ValueError('pinned empty-created fixture changed')
        snapshot = output / 'source'
        shutil.copytree(ROOT / 'include', snapshot / 'include')
        files = SOURCES + [
            'src/arch/ops_private.hpp',
            'tests/host/ext4_realtime_model.hpp',
            'tests/host/ext4_mounted_next_data_tests.cpp',
            'tests/host/ext4_owned_next_data_tests.cpp',
            'tests/host/ext4_next_data_compile.cpp',
            'tests/host/ext4_mounted_support.hpp',
            'tests/host/ext4_owned_support.hpp',
            'tests/host/ext4_transaction_support.hpp',
            'scripts/test_ext4_next_data.py',
        ]
        for name in files:
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        report['source_sha256'] = {str(p.relative_to(snapshot)): digest(p)
                                   for p in sorted(snapshot.rglob('*')) if p.is_file()}
        fixture = output / 'empty-created-input.img'
        shutil.copyfile(original, fixture)
        report['original_sha256'] = digest(original)
        report['input_sha256'] = digest(fixture)
        run(['e2fsck', '-fn', fixture], 'input-fsck')
        flags = ['-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-fno-exceptions', '-frtti', '-ffunction-sections', '-fdata-sections',
                 '-Wl,--gc-sections', '-DOK_ARCH_TARGET_X86_64', '-I' + str(snapshot / 'include')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        executable = output / 'mounted-next-data-tests'
        run(['g++', *flags, snapshot / 'tests/host/ext4_mounted_next_data_tests.cpp',
             *(snapshot / name for name in SOURCES), '-Wl,-l:libext2fs.so.2', '-pthread',
             '-o', executable], 'compile-mounted')
        print(run([executable, fixture, output / 'first-block.img'], 'mounted'), end='', flush=True)
        completed = output / 'first-block.completed.img'
        run(['e2fsck', '-fn', completed], 'completed-fsck')
        run(['debugfs', '-R', 'stat /empty-note', completed], 'completed-inode')
        run(['e2fsck', '-fn', output / 'first-block.capacity.img'], 'capacity-fsck')
        run(['e2fsck', '-fn', output / 'first-block.sibling.img'], 'sibling-fsck')
        owner = output / 'owned-next-data-tests'
        run(['g++', *flags, snapshot / 'tests/host/ext4_owned_next_data_tests.cpp',
             *(snapshot / name for name in SOURCES), '-Wl,-l:libext2fs.so.2', '-pthread',
             '-o', owner], 'compile-owner')
        print(run([owner, output / 'first-block.img', output / 'owner-model'], 'owner'), end='', flush=True)
        run(['e2fsck', '-fn', output / 'owner-model/completed.img'], 'owner-completed-fsck')
        reference = output / 'independent-replay.img'
        shutil.copyfile(output / 'owner-model/published.img', reference)
        run(['e2fsck', '-fy', '-E', 'journal_only', reference], 'independent-replay', (0, 1))
        run(['e2fsck', '-fn', reference], 'independent-fsck')
        actual = (output / 'owner-model/completed.img').read_bytes()
        independent = reference.read_bytes()
        differences = [at for at in range(len(actual)) if actual[at] != independent[at]]
        bookkeeping = set(range(1024 + 48, 1024 + 54)) | set(range(1024 + 64, 1024 + 68)) | \
            set(range(1024 + 96, 1024 + 100)) | set(range(1024 + 376, 1024 + 384)) | \
            set(range(2044, 2048))
        if any(at not in bookkeeping for at in differences):
            raise ValueError('independent replay differs outside explicit ext4 bookkeeping')
        report['independent_bookkeeping_differences'] = differences
        print(run([owner, output / 'first-block.sibling.img', output / 'owner-sibling-model'],
                  'owner-sibling'), end='', flush=True)
        run(['e2fsck', '-fn', output / 'owner-sibling-model/completed.img'],
            'owner-sibling-completed-fsck')
        sibling_reference = output / 'sibling-independent-replay.img'
        shutil.copyfile(output / 'owner-sibling-model/published.img', sibling_reference)
        run(['e2fsck', '-fy', '-E', 'journal_only', sibling_reference],
            'sibling-independent-replay', (0, 1))
        run(['e2fsck', '-fn', sibling_reference], 'sibling-independent-fsck')
        sibling_actual = (output / 'owner-sibling-model/completed.img').read_bytes()
        sibling_independent = sibling_reference.read_bytes()
        sibling_differences = [at for at in range(len(sibling_actual))
                               if sibling_actual[at] != sibling_independent[at]]
        if any(at not in bookkeeping for at in sibling_differences):
            raise ValueError('sibling independent replay differs outside explicit ext4 bookkeeping')
        report['sibling_bookkeeping_differences'] = sibling_differences
        if args.cross:
            toolchains = (args.toolchain_root or ROOT / 'toolchains').resolve()
            for arch, triple, elf_class, endian, machine in PROFILES:
                compiler = toolchains / triple / 'bin' / (triple + '-g++')
                if not compiler.is_file():
                    raise ValueError('explicit cross compiler absent: ' + str(compiler))
                obj = output / (arch + '.o')
                run([compiler, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                     '-ffreestanding', '-fno-exceptions', '-fno-threadsafe-statics',
                     '-fno-use-cxa-atexit', '-frtti', *TARGET_FLAGS[arch],
                     '-DOK_ARCH_TARGET_' + arch.upper(), '-I' + str(snapshot / 'include'),
                     '-c', snapshot / 'tests/host/ext4_next_data_compile.cpp', '-o', obj],
                    'cross-' + arch)
                data = obj.read_bytes()[:20]
                order = '<' if endian == 1 else '>'
                if data[:6] != b'\x7fELF' + bytes((elf_class, endian)) or \
                        struct.unpack_from(order + 'H', data, 18)[0] != machine:
                    raise ValueError('cross object ELF identity mismatch: ' + arch)
                report.setdefault('cross_object_sha256', {})[arch] = digest(obj)
        report['input_unchanged'] = digest(original) == report['original_sha256'] and \
            digest(fixture) == report['input_sha256']
        if not report['input_unchanged']:
            raise ValueError('pinned input changed')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        print('FAIL ' + str(error), file=sys.stderr)
    finally:
        report['artifact_sha256'] = {str(p.relative_to(output)): digest(p)
                                     for p in sorted(output.rglob('*'))
                                     if p.is_file() and p.name != 'result-v1.json'}
        save()
    if report['status'] != 'passed':
        raise SystemExit(1)
    print('PASS hosted next-data mounted', digest(output / 'result-v1.json'))


if __name__ == '__main__':
    main()
