#!/usr/bin/env python3
"""One-shot explicit-witness namespace replay model and ten-profile compile receipt."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import traceback

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    'include/ok/fs/ext4/dir.hpp',
    'include/ok/fs/ext4/recovery.hpp',
    'src/fs/ext4_recovery.cpp',
    'tests/host/ext4_namespace_journal_fixture.hpp',
    'tests/host/ext4_namespace_replay_tests.cpp',
    'tests/host/ext4_namespace_replay_compile.cpp',
    'scripts/test_ext4_owned.py',
    'scripts/test_ext4_namespace_replay.py',
)


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def need(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def save(path: Path, report: dict) -> None:
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')


def main() -> None:
    need(__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1',
         'use non-optimized bytecode-disabled Python')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--filled-image', type=Path, required=True)
    parser.add_argument('--filled-sha256', required=True)
    parser.add_argument('--empty-image', type=Path, required=True)
    parser.add_argument('--empty-sha256', required=True)
    parser.add_argument('--source-hashes', type=Path, required=True)
    parser.add_argument('--source-hashes-sha256', required=True)
    parser.add_argument('--host-cxx', type=Path, required=True)
    parser.add_argument('--host-cxx-sha256', required=True)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    args = parser.parse_args()
    output = args.output
    need(output.is_absolute() and output.resolve() == output and not output.exists() and
         not output.is_relative_to(ROOT) and not ROOT.is_relative_to(output),
         'output must be a new absolute directory outside Kernel source')
    need(args.source_hashes.is_absolute() and args.source_hashes.resolve() == args.source_hashes and
         sha(args.source_hashes) == args.source_hashes_sha256,
         'reviewed source pin manifest changed')
    pins = json.loads(args.source_hashes.read_text())
    need(pins.get('schema') == 1 and pins.get('status') == 'reviewed-namespace-replay-model-inputs' and
         set(pins.get('source_sha256', {})) == set(FILES),
         'source pin manifest has an unexpected file set')
    need(pins.get('fixtures') == {
             'filled': {'path': str(args.filled_image), 'sha256': args.filled_sha256},
             'empty': {'path': str(args.empty_image), 'sha256': args.empty_sha256},
         } and pins.get('host_compiler') ==
         {'path': str(args.host_cxx), 'sha256': args.host_cxx_sha256} and
         pins.get('toolchain_root') == str(args.toolchain_root),
         'fixture or compiler tuple differs from reviewed pins')
    for name, expected in pins['source_sha256'].items():
        source = ROOT / name
        need(source.is_file() and not source.is_symlink() and sha(source) == expected,
             'source changed before snapshot: ' + name)
    for path, expected in ((args.filled_image, args.filled_sha256),
                           (args.empty_image, args.empty_sha256),
                           (args.host_cxx, args.host_cxx_sha256)):
        need(path.is_absolute() and path.resolve() == path and path.is_file() and
             not path.is_symlink() and sha(path) == expected,
             'exact fixture/compiler input changed: ' + str(path))
    need(args.toolchain_root.is_absolute() and args.toolchain_root.resolve() == args.toolchain_root,
         'toolchain root must be explicit and absolute')
    output.mkdir(parents=True, exist_ok=False)
    receipt = output / 'result.json'
    report = {
        'schema': 1, 'status': 'running-explicit-witness-namespace-replay-model',
        'scope': 'pure checked-old-witness namespace journal/home semantics',
        'source_pins_sha256': args.source_hashes_sha256,
        'filled_fixture_sha256': args.filled_sha256,
        'empty_fixture_sha256': args.empty_sha256,
        'host_compiler_sha256': args.host_cxx_sha256,
        'host_sanitizers': ['address', 'undefined'],
        'sanitizer_leak_detection': False,
        'profiles': [], 'commands': [], 'files': {},
        'hardware': False, 'qemu': False, 'device_writes': False,
        'cold_all_after_authority': False, 'mounted_integration': False,
        'full_m0_m12': False,
    }
    save(receipt, report)

    def run(label: str, command: list[str | Path], environment: dict | None = None) -> str:
        argv = [str(value) for value in command]
        log = output / (label + '.log')
        with log.open('x') as stream:
            result = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT,
                                    env=environment, timeout=180)
        report['commands'].append({'label': label, 'argv': argv,
                                   'returncode': result.returncode, 'log_sha256': sha(log)})
        save(receipt, report)
        need(result.returncode == 0, label + ' failed; retain log')
        return log.read_text()

    try:
        snapshot = output / 'source' / 'ObfuscationKernel'
        shutil.copytree(ROOT / 'include', snapshot / 'include')
        for name in FILES:
            original = ROOT / name
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copyfile(original, target)
            need(sha(target) == pins['source_sha256'][name],
                 'snapshot differs from reviewed source: ' + name)
            report['files']['source/ObfuscationKernel/' + name] = sha(target)
        for source, label, expected in ((args.filled_image, 'filled.img', args.filled_sha256),
                                        (args.empty_image, 'empty.img', args.empty_sha256)):
            target = output / label
            shutil.copyfile(source, target)
            need(sha(target) == expected, 'copied fixture differs: ' + label)
            report['files'][label] = expected
        save(receipt, report)
        module_spec = importlib.util.spec_from_file_location('replay_model_profiles',
                                                               snapshot / 'scripts/test_ext4_owned.py')
        need(module_spec is not None and module_spec.loader is not None,
             'ten-profile compiler table missing')
        profiles = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(profiles)
        need(len(profiles.PROFILES) == 10 and
             {row[0] for row in profiles.PROFILES} ==
             {'i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32',
              'loongarch64', 'mips', 'mips64', 'ppc'},
             'ten-profile matrix narrowed')
        host_flags = ['-std=c++23', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                      '-fno-exceptions', '-I' + str(snapshot / 'include')]
        tests = snapshot / 'tests/host/ext4_namespace_replay_tests.cpp'
        recovery = snapshot / 'src/fs/ext4_recovery.cpp'
        host = output / 'host-model'
        run('host-compile', [args.host_cxx, *host_flags, tests, recovery, '-o', host])
        report['files']['host-model'] = sha(host)
        expected_text = 'PASS explicit-witness namespace replay mixtures=144 rejected=76 no_device_io=1 cold_authority=0'
        need(expected_text in run('host-run', [host, output / 'filled.img', output / 'empty.img']),
             'host coverage marker differs')
        sanitized = output / 'sanitized-model'
        run('sanitized-compile', [args.host_cxx, *host_flags,
                                  '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                  tests, recovery, '-o', sanitized])
        report['files']['sanitized-model'] = sha(sanitized)
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1')
        need(expected_text in run('sanitized-run',
             [sanitized, output / 'filled.img', output / 'empty.img'], environment),
             'sanitized coverage marker differs')
        cross = snapshot / 'tests/host/ext4_namespace_replay_compile.cpp'
        for arch, triple, elf_class, endian, machine in profiles.PROFILES:
            compiler = args.toolchain_root / triple / 'bin' / (triple + '-g++')
            need(compiler.is_file() and not compiler.is_symlink(),
                 'cross compiler absent: ' + arch)
            obj = output / (arch + '.o')
            run(arch + '-compile', [compiler, '-std=c++23', '-O2', '-Wall', '-Wextra',
                 '-Wpedantic', '-Werror', '-ffreestanding', '-fno-exceptions',
                 '-fno-threadsafe-statics', '-fno-use-cxa-atexit', '-frtti',
                 *profiles.TARGET_FLAGS[arch], '-DOK_ARCH_TARGET_' + arch.upper(),
                 '-I' + str(snapshot / 'include'), '-c', cross, '-o', obj])
            payload = obj.read_bytes()
            need(payload[:4] == b'\x7fELF' and list(payload[4:6]) == [elf_class, endian] and
                 struct.unpack_from('<H' if endian == 1 else '>H', payload, 18)[0] == machine,
                 'cross object architecture differs: ' + arch)
            report['profiles'].append(arch)
            report['commands'][-1].update(compiler_sha256=sha(compiler), object_sha256=sha(obj))
            report['files'][obj.name] = sha(obj)
            save(receipt, report)
        for name, expected in pins['source_sha256'].items():
            need(sha(ROOT / name) == expected, 'reviewed source changed during run: ' + name)
        need(sha(args.filled_image) == args.filled_sha256 and
             sha(args.empty_image) == args.empty_sha256 and
             sha(args.source_hashes) == args.source_hashes_sha256,
             'pinned input changed during run')
        report['status'] = 'passed-explicit-witness-pure-model-ten-arch'
        report['mixtures'] = 144
        report['malformed_and_invalid_rejections'] = 76
        report['ten_profile_objects'] = True
        save(receipt, report)
        print('PASS explicit-witness namespace replay model', sha(receipt), flush=True)
    except BaseException as error:
        report['status'] = 'failed-preserved-explicit-witness-namespace-replay-model'
        report['error'] = repr(error)
        trace = output / 'failure.traceback.log'
        trace.write_text(traceback.format_exc())
        report['failure_traceback_sha256'] = sha(trace)
        save(receipt, report)
        raise


if __name__ == '__main__':
    main()
