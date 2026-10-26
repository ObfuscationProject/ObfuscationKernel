#!/usr/bin/env python3
"""One-shot pinned orphan-access model, sanitizers and ten-profile compile receipt."""

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
import traceback

ROOT = Path(__file__).resolve().parents[1]
HEADERS = (
    'include/ok/arch/arch.hpp',
    'include/ok/arch/execution.hpp',
    'include/ok/arch/kernel_entry_stack.hpp',
    'include/ok/arch/native.hpp',
    'include/ok/arch/profiles.hpp',
    'include/ok/arch/types.hpp',
    'include/ok/core/concepts.hpp',
    'include/ok/core/fixed.hpp',
    'include/ok/core/types.hpp',
    'include/ok/driver/driver.hpp',
    'include/ok/fs/attributes.hpp',
    'include/ok/fs/ext4_orphan_access.hpp',
    'include/ok/fs/ext4_orphan_chain.hpp',
    'include/ok/fs/ext4_readonly.hpp',
    'include/ok/fs/ext4_recovery.hpp',
    'include/ok/fs/ext4_times.hpp',
    'include/ok/fs/jbd2.hpp',
    'include/ok/fs/jbd2_replay.hpp',
    'include/ok/fs/mode_change.hpp',
    'include/ok/fs/vfs.hpp',
    'include/ok/memory/kernel_entry_stack_pool.hpp',
    'include/ok/memory/memory.hpp',
    'include/ok/sched/deadline.hpp',
    'include/ok/sched/resources.hpp',
    'include/ok/sched/scheduler.hpp',
    'include/ok/sched/signal_mask.hpp',
    'include/ok/sched/wait_registration.hpp',
    'include/ok/smp/smp.hpp',
    'include/ok/user/native_mapping_pages.hpp',
    'include/ok/user/native_page_pool.hpp',
    'include/ok/user/native_signal_config.hpp',
    'include/ok/user/user.hpp',
)
FILES = (
    'src/fs/ext4_recovery.cpp',
    'tests/host/ext4_orphan_access_fixture.hpp',
    'tests/host/ext4_orphan_access_tests.cpp',
    'tests/host/ext4_orphan_access_compile.cpp',
    'scripts/test_ext4_orphan_access.py',
)
PROFILES = (
    ('i386', 'i386-elf', 1, 1, 3, ('-mno-sse', '-mno-sse2')),
    ('x86_64', 'x86_64-elf', 2, 1, 62, ('-mno-red-zone',)),
    ('aarch64', 'aarch64-elf', 2, 1, 183, ('-mno-outline-atomics', '-mgeneral-regs-only', '-mstrict-align')),
    ('arm32', 'arm-none-eabi', 1, 1, 40, ('-march=armv7-a', '-marm', '-mno-unaligned-access')),
    ('rv64', 'riscv64-elf', 2, 1, 243, ('-mcmodel=medany',)),
    ('rv32', 'riscv32-elf', 1, 1, 243, ('-mcmodel=medany',)),
    ('loongarch64', 'loongarch64-elf', 2, 1, 258, ()),
    ('mips', 'mips-elf', 1, 2, 8, ('-march=mips32r2', '-G0')),
    ('mips64', 'mips64-elf', 2, 2, 8, ('-march=mips64r2', '-mabi=64', '-G0')),
    ('ppc', 'powerpc-eabi', 1, 2, 20, ('-msoft-float', '-msdata=none')),
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
    path.write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')


def main() -> None:
    need(__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1',
         'use non-optimized bytecode-disabled Python')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-hashes', type=Path, required=True)
    parser.add_argument('--source-hashes-sha256', required=True)
    parser.add_argument('--empty-image', type=Path, required=True)
    parser.add_argument('--empty-sha256', required=True)
    parser.add_argument('--filled-image', type=Path, required=True)
    parser.add_argument('--filled-sha256', required=True)
    parser.add_argument('--host-cxx', type=Path, required=True)
    parser.add_argument('--host-cxx-sha256', required=True)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    args = parser.parse_args()
    output = args.output
    need(output.is_absolute() and output.resolve() == output and not output.exists() and
         not output.is_relative_to(ROOT) and not ROOT.is_relative_to(output),
         'output must be a new absolute directory outside Kernel source')
    need(args.source_hashes.is_absolute() and args.source_hashes.resolve() == args.source_hashes and
         args.source_hashes.is_file() and not args.source_hashes.is_symlink() and
         sha(args.source_hashes) == args.source_hashes_sha256,
         'reviewed source pin manifest changed')
    pins = json.loads(args.source_hashes.read_text())
    expected_names = set(HEADERS) | set(FILES)
    need(pins.get('schema') == 1 and
         pins.get('status') == 'reviewed-orphan-access-model-inputs' and
         set(pins.get('source_sha256', {})) == expected_names and
         pins.get('fixtures') == {
             'empty': {'path': str(args.empty_image), 'sha256': args.empty_sha256},
             'filled': {'path': str(args.filled_image), 'sha256': args.filled_sha256},
         } and
         pins.get('host_compiler') ==
         {'path': str(args.host_cxx), 'sha256': args.host_cxx_sha256} and
         pins.get('toolchain_root') == str(args.toolchain_root),
         'reviewed source, fixture or compiler tuple differs')
    for name, expected in pins['source_sha256'].items():
        source = ROOT / name
        need(source.is_file() and not source.is_symlink() and sha(source) == expected,
             'source changed before snapshot: ' + name)
    for path, expected in ((args.empty_image, args.empty_sha256),
                           (args.filled_image, args.filled_sha256),
                           (args.host_cxx, args.host_cxx_sha256)):
        need(path.is_absolute() and path.resolve() == path and path.is_file() and
             not path.is_symlink() and sha(path) == expected,
             'exact fixture/compiler input changed: ' + str(path))
    need(args.toolchain_root.is_absolute() and args.toolchain_root.resolve() == args.toolchain_root,
         'toolchain root must be explicit and absolute')
    output.mkdir(parents=True, exist_ok=False)
    receipt = output / 'result.json'
    report = {
        'schema': 1, 'status': 'running-pinned-orphan-access-model',
        'scope': 'pure complete-chain-gated read-only inode snapshot',
        'source_pins_sha256': args.source_hashes_sha256,
        'empty_fixture_sha256': args.empty_sha256,
        'filled_fixture_sha256': args.filled_sha256,
        'host_compiler_sha256': args.host_cxx_sha256,
        'host_sanitizers': ['address', 'undefined'], 'sanitizer_leak_detection': False,
        'profiles': [], 'commands': [], 'files': {},
        'hardware': False, 'qemu': False, 'device_writes': False,
        'mounted_integration': False, 'retained_descriptor_lifetime': False,
        'orphan_reclaim': False, 'full_m0_m12': False,
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
        for name in (*HEADERS, *FILES):
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        for name, expected in pins['source_sha256'].items():
            target = snapshot / name
            need(sha(target) == expected, 'snapshot differs from reviewed source: ' + name)
            report['files']['source/ObfuscationKernel/' + name] = expected
        for source, label, expected in ((args.empty_image, 'empty.img', args.empty_sha256),
                                        (args.filled_image, 'filled.img', args.filled_sha256)):
            target = output / label
            shutil.copyfile(source, target)
            need(sha(target) == expected, 'copied fixture differs: ' + label)
            report['files'][label] = expected
        save(receipt, report)
        host_flags = ['-std=c++23', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                      '-fno-exceptions', '-I' + str(snapshot / 'include')]
        tests = snapshot / 'tests/host/ext4_orphan_access_tests.cpp'
        recovery = snapshot / 'src/fs/ext4_recovery.cpp'
        host = output / 'host-model'
        run('host-compile', [args.host_cxx, *host_flags, tests, recovery, '-o', host])
        report['files']['host-model'] = sha(host)
        marker = 'PASS orphan read-only snapshot: populated, empty, rejections=11 no_device_io=1'
        need(marker in run('host-run', [host, output / 'empty.img', output / 'filled.img']),
             'host coverage marker differs')
        sanitized = output / 'sanitized-model'
        run('sanitized-compile', [args.host_cxx, *host_flags, '-g', '-fsanitize=address,undefined',
                                  '-fno-omit-frame-pointer', tests, recovery, '-o', sanitized])
        report['files']['sanitized-model'] = sha(sanitized)
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1')
        need(marker in run('sanitized-run',
                           [sanitized, output / 'empty.img', output / 'filled.img'], environment),
             'sanitized coverage marker differs')
        cross = snapshot / 'tests/host/ext4_orphan_access_compile.cpp'
        for arch, triple, elf_class, endian, machine, arch_flags in PROFILES:
            compiler = args.toolchain_root / triple / 'bin' / (triple + '-g++')
            need(compiler.is_file() and not compiler.is_symlink(),
                 'cross compiler absent: ' + arch)
            obj = output / (arch + '.o')
            run(arch + '-compile', [compiler, '-std=c++23', '-O2', '-Wall', '-Wextra',
                '-Wpedantic', '-Werror', '-ffreestanding', '-fno-exceptions',
                '-fno-threadsafe-statics', '-fno-use-cxa-atexit', '-frtti',
                *arch_flags, '-DOK_ARCH_TARGET_' + arch.upper(),
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
        need(sha(args.empty_image) == args.empty_sha256 and
             sha(args.filled_image) == args.filled_sha256 and
             sha(args.source_hashes) == args.source_hashes_sha256,
             'pinned input changed during run')
        report['status'] = 'passed-pinned-pure-model-ten-arch'
        report['malformed_and_invalid_rejections'] = 11
        report['ten_profile_objects'] = True
        save(receipt, report)
        print('PASS orphan-access pure model', sha(receipt), flush=True)
    except BaseException as error:
        report['status'] = 'failed-preserved-pinned-orphan-access-model'
        report['error'] = repr(error)
        trace = output / 'failure.traceback.log'
        trace.write_text(traceback.format_exc())
        report['failure_traceback_sha256'] = sha(trace)
        save(receipt, report)
        raise


if __name__ == '__main__':
    main()
