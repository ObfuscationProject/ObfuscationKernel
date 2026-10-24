#!/usr/bin/env python3
"""Qualify owner-bound namespace cold admission and checkpoint with a memory device."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

from test_ext4_owned import PROFILES, TARGET_FLAGS

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--empty-image', type=Path, required=True)
    parser.add_argument('--empty-sha256', required=True)
    parser.add_argument('--image-sha256', required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cross', action='store_true')
    parser.add_argument('--toolchain-root', type=Path)
    args = parser.parse_args()
    if not (__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1'):
        parser.error('use PYTHONDONTWRITEBYTECODE=1 python3 -B')
    image = args.image.resolve()
    empty = args.empty_image.resolve()
    if not empty.is_file() or sha(empty) != args.empty_sha256:
        parser.error('exact empty fixture hash differs')
    if not image.is_file() or sha(image) != args.image_sha256:
        parser.error('exact fixture hash differs')
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be separate from the Kernel source tree')
    output.mkdir(parents=True, exist_ok=False)
    snapshot = output / 'source'
    shutil.copytree(ROOT / 'include', snapshot / 'include')
    files = ['tests/host/ext4_namespace_completion_tests.cpp',
             'tests/host/ext4_namespace_completion_compile.cpp',
             'tests/host/ext4_namespace_journal_fixture.hpp',
             'tests/host/ext4_owned_support.hpp',
             'tests/host/ext4_transaction_support.hpp',
             'src/fs/ext4_recovery.cpp',
             'scripts/test_ext4_namespace_completion.py',
             'scripts/test_ext4_owned.py']
    for name in files:
        target = snapshot / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)
    retained_image = output / 'input.img'
    shutil.copyfile(image, retained_image)
    if sha(retained_image) != args.image_sha256:
        raise ValueError('fixture changed during copy')
    retained_empty = output / 'empty.img'
    shutil.copyfile(empty, retained_empty)
    if sha(retained_empty) != args.empty_sha256:
        raise ValueError('empty fixture changed during copy')
    report = dict(schema=1, status='running', private_namespace_owner=True, memory_device=True,
        mounted_namespace=False, cold_admission=True, checkpoint=True,
        orphan_reclaim=False, descriptor_lifetime=False, sdk_export=False,
        hardware=False, qemu=False, full_m0_m12=False, sanitizer=args.sanitize,
        sanitizer_leak_detection=False if args.sanitize else None,
        input_sha256=args.image_sha256, empty_sha256=args.empty_sha256, commands=[],
        source_sha256={str(path.relative_to(snapshot)): sha(path)
                       for path in sorted(snapshot.rglob('*')) if path.is_file()})

    def run(label, argv, environment=None):
        argv = [str(item) for item in argv]
        with (output / (label + '.log')).open('xb') as log:
            result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=600, env=environment)
        report['commands'].append(dict(label=label, argv=argv, returncode=result.returncode,
                                       log_sha256=sha(output / (label + '.log'))))
        if result.returncode:
            raise RuntimeError(label + ' failed; inspect retained log')

    try:
        flags = ['-std=c++23', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-fno-exceptions', '-frtti', '-DOK_ARCH_TARGET_X86_64',
                 '-I' + str(snapshot / 'include')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        executable = output / 'completion-model'
        compiler = Path('/usr/bin/x86_64-linux-gnu-g++-13')
        report['compiler_sha256'] = sha(compiler)
        run('compile-hosted', [compiler, *flags, snapshot / files[0], snapshot / files[5],
                               '-Wl,-l:libext2fs.so.2', '-pthread', '-o', executable])
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1') if args.sanitize else None
        report['binary_sha256'] = sha(executable)
        run('hosted', [executable, retained_image, retained_empty], environment)
        import re
        summary = (output / 'hosted.log').read_text()
        values = dict((name, int(value)) for name, value in re.findall(r'(mixtures|live|all_after|faults|rejected|read_faults|history_rejected)=(\d+)', summary))
        if values != dict(mixtures=112, live=7, all_after=7, faults=54, rejected=5, read_faults=8, history_rejected=1):
            raise ValueError('executed model summary differs')
        report['executed_cases'] = values
        if args.cross:
            toolchains = (args.toolchain_root or ROOT / 'toolchains').resolve()
            for arch, triple, elf_class, endian, machine in PROFILES:
                compiler = toolchains / triple / 'bin' / (triple + '-g++')
                obj = output / (arch + '.o')
                run(arch, [compiler, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-ffreestanding', '-fno-exceptions', '-fno-threadsafe-statics',
                    '-fno-use-cxa-atexit', '-frtti', *TARGET_FLAGS[arch],
                    '-DOK_ARCH_TARGET_' + arch.upper(), '-I' + str(snapshot / 'include'),
                    '-c', snapshot / files[1], '-o', obj])
                payload = obj.read_bytes()
                if payload[:4] != b'\x7fELF' or list(payload[4:6]) != [elf_class, endian] or \
                   struct.unpack_from('<H' if endian == 1 else '>H', payload, 18)[0] != machine:
                    raise ValueError('cross architecture object mismatch: ' + arch)
                report['commands'][-1].update(compiler_sha256=sha(compiler), object_sha256=sha(obj))
        if sha(retained_image) != args.image_sha256 or sha(retained_empty) != args.empty_sha256:
            raise ValueError('retained inputs changed')
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
        raise
    finally:
        with (output / 'result.json').open('x') as stream:
            stream.write(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print('PASS owner-bound namespace completion model', sha(output / 'result.json'), flush=True)


if __name__ == '__main__':
    main()
