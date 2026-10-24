#!/usr/bin/env python3
"""Run prior owned ext4 hosted operations against one frozen namespace-owner source snapshot."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    for name in ('clean', 'empty', 'first_block'):
        parser.add_argument('--' + name.replace('_', '-') + '-image', type=Path, required=True)
        parser.add_argument('--' + name.replace('_', '-') + '-sha256', required=True)
    args = parser.parse_args()
    if not (__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1'):
        parser.error('use PYTHONDONTWRITEBYTECODE=1 python3 -B')
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error('output must be separate from the Kernel source tree')
    input_paths = {name: getattr(args, name + '_image').resolve()
                   for name in ('clean', 'empty', 'first_block')}
    expected = {name: getattr(args, name + '_sha256') for name in input_paths}
    for name, path in input_paths.items():
        if not path.is_file() or sha(path) != expected[name]:
            parser.error(name + ' exact input hash differs')
    output.mkdir(parents=True, exist_ok=False)
    snapshot = output / 'source'
    shutil.copytree(ROOT / 'include', snapshot / 'include')
    tests = ('ext4_owned_tests', 'ext4_owned_rename_fault_tests',
             'ext4_owned_create_tests', 'ext4_owned_first_data_tests',
             'ext4_owned_next_data_tests')
    files = ['tests/host/' + name + '.cpp' for name in tests]
    files += ['tests/host/ext4_owned_support.hpp', 'tests/host/ext4_transaction_support.hpp',
              'tests/host/ext4_realtime_model.cpp', 'tests/host/ext4_realtime_model.hpp',
              'src/fs/ext4_recovery.cpp', 'scripts/test_ext4_namespace_owner_regressions.py']
    for name in files:
        target = snapshot / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)
    images = {}
    for name, path in input_paths.items():
        retained = output / (name + '.img')
        shutil.copyfile(path, retained)
        if sha(retained) != expected[name]:
            raise ValueError(name + ' input changed during copy')
        images[name] = retained
    report = dict(schema=1, status='running', prior_owned_hosted_regressions=True,
        namespace_checkpoint=False, mounted_namespace=False, qemu=False, hardware=False,
        full_m0_m12=False, input_sha256=expected, commands=[],
        source_sha256={str(path.relative_to(snapshot)): sha(path)
                       for path in sorted(snapshot.rglob('*')) if path.is_file()})

    def run(label, argv):
        argv = [str(item) for item in argv]
        with (output / (label + '.log')).open('xb') as log:
            result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        report['commands'].append(dict(label=label, argv=argv, returncode=result.returncode,
                                       log_sha256=sha(output / (label + '.log'))))
        if result.returncode:
            raise RuntimeError(label + ' failed; inspect retained log')

    try:
        flags = ['-std=c++23', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-fno-exceptions', '-frtti', '-DOK_ARCH_TARGET_X86_64',
                 '-I' + str(snapshot / 'include')]
        arguments = {
            'ext4_owned_tests': [images['clean'], output / 'metadata-model'],
            'ext4_owned_rename_fault_tests': [images['clean']],
            'ext4_owned_create_tests': [images['clean'], output / 'create-model'],
            'ext4_owned_first_data_tests': [images['empty'], output / 'first-data-model'],
            'ext4_owned_next_data_tests': [images['first_block'], output / 'next-data-model'],
        }
        for name in tests:
            binary = output / name
            run('compile-' + name, ['g++', *flags, snapshot / ('tests/host/' + name + '.cpp'),
                snapshot / 'tests/host/ext4_realtime_model.cpp',
                snapshot / 'src/fs/ext4_recovery.cpp', '-Wl,-l:libext2fs.so.2',
                '-pthread', '-o', binary])
            run(name, [binary, *arguments[name]])
        for name, path in images.items():
            if sha(path) != expected[name]:
                raise ValueError(name + ' retained input changed')
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
        raise
    finally:
        with (output / 'result.json').open('x') as stream:
            stream.write(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print('PASS prior owned ext4 hosted regressions', sha(output / 'result.json'), flush=True)


if __name__ == '__main__':
    main()
