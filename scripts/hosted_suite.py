#!/usr/bin/env python3
"""Run the hosted and QEMU runners under scripts/test_*.py as one suite.

Every runner is classified here; the suite refuses a runner whose required
arguments it does not know, so a new runner cannot drift out of CI unnoticed.

  self       needs nothing but a fresh --output directory (when it takes one)
  toolchain  needs the SysToolchain cross compilers and QEMU; runs only with
             --toolchain-root DIR (a directory holding the ten <triple>/ trees)
  fixture    needs the clean journalled ext4 image; runs only with --ext4-fixture,
             which builds it with e2fsprogs (scripts/make_clean_ext4_fixture.py)
  inputs     needs explicit images, oracles, ELFs or SDKs; listed with what it
             needs and never run by this suite
  quarantine fails on main for a recorded reason; run with --quarantined

Model runners (scope=model) and QEMU probes (scope=hardware) keep their own
labels; passing this suite claims neither guest userland nor physical hardware.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'scripts'
TRIPLES = {'i386': 'i386-elf', 'x86_64': 'x86_64-elf', 'aarch64': 'aarch64-elf', 'arm32': 'arm-none-eabi',
           'rv64': 'riscv64-elf', 'rv32': 'riscv32-elf', 'loongarch64': 'loongarch64-elf',
           'mips': 'mips-elf', 'mips64': 'mips64-elf', 'ppc': 'powerpc-eabi'}
QEMU = {'i386': 'qemu-system-i386', 'x86_64': 'qemu-system-x86_64', 'aarch64': 'qemu-system-aarch64',
        'arm32': 'qemu-system-arm', 'rv64': 'qemu-system-riscv64', 'rv32': 'qemu-system-riscv32',
        'loongarch64': 'qemu-system-loongarch64', 'mips': 'qemu-system-mips', 'mips64': 'qemu-system-mips64',
        'ppc': 'qemu-system-ppc'}


def probe(arch):
    """Arguments of the per-architecture QEMU execution probe test_native_<arch>.py."""
    return ['--toolchain', '{tc}/' + TRIPLES[arch], '--qemu', '{tc}/' + TRIPLES[arch] + '/bin/' + QEMU[arch]]


# Runners that need a cross toolchain: the argument template, '{tc}' is the toolchain root.
TOOLCHAIN = {
    **{f'native_{arch}': probe(arch) for arch in
       ('i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32', 'loongarch64', 'mips', 'ppc')},
    'native_signal_frame_aarch64': ['--toolchain', '{tc}/aarch64-elf'],
    'mount_info_uapi_cross': ['--toolchain-root', '{tc}'],
    'persistent_sync_cross': ['--toolchain-root', '{tc}'],
    'power_uapi_cross': ['--toolchain-root', '{tc}'],
    'shutdown_cross': ['--toolchain-root', '{tc}'],
    'linux_bootstrap': ['--toolchain-root', '{tc}'],
    'service_stack': ['--toolchains', '{tc}'],
}

# Runners whose inputs the suite generates itself with --ext4-fixture (e2fsprogs):
# '{clean_image}' is the clean journalled image from make_clean_ext4_fixture.py.
FIXTURE = {name: ['--clean-image', '{clean_image}'] for name in (
    'checked_ownership', 'ext4_directory_mode', 'ext4_journalled_mode', 'ext4_mounted',
    'ext4_mounted_create', 'ext4_owned', 'ext4_owned_edges', 'ext4_transaction')}

EXT4_EVIDENCE = ('hash-pinned ext4 images from a prior slice receipt, the source-hash manifest, '
                 'host g++ and toolchain root (ObfuscationEvidence receipts)')
# Runners with explicit inputs this suite does not fabricate.
INPUTS = {
    **{name: 'Linux oracle record (--oracle) captured by the ObfuscationOS Linux stat/chmod tools'
       for name in ('linux_chmod_provider', 'linux_chmod_service', 'linux_stat_copy_order',
                    'linux_stat_provider', 'linux_stat_service')},
    **{name: 'hash-pinned ext4 image (--image, --image-sha256) from the namespace slice receipts'
       for name in ('ext4_namespace_journal', 'ext4_namespace_recovery_binding', 'ext4_namespace_transaction')},
    **{name: EXT4_EVIDENCE for name in (
        'ext4_orphan_access', 'ext4_orphan_ownership', 'ext4_orphan_reclaim_plan',
        'ext4_namespace_cold_replay', 'ext4_namespace_replay', 'ext4_inode_lifetime')},
    **{name: 'toolchain root plus a kernel SDK built with the named feature flag (--flag-sdk)'
       for name in ('linux_access_targets', 'linux_chmod_targets', 'linux_owned_stat_bootstrap_targets')},
    **{name: 'hash-pinned PPC model ELF (--ppc-elf, --ppc-elf-sha256) built by ObfuscationOS'
       for name in ('native_futex_origin', 'native_pipe_vector_publication', 'native_syscall_restart_publication')},
    **{name: 'hash-pinned PPC model ELF source (--ppc-model-elf-source) built by ObfuscationOS'
       for name in ('linux_udp_restart', 'native_restart_regressions')},
    'ext4_completion': 'recovery fixture directory (--fixture) from the completion slice receipt',
    'persistent_sync': 'persistent sync fixture (--fixture) from ObfuscationOS',
    'ext4_first_data': 'created empty ext4 image (--created-empty-image) from test_ext4_mounted_create',
    'ext4_next_data': 'empty created ext4 image (--empty-created-image) from test_ext4_mounted_create',
    'ext4_namespace_completion': 'namespace image and its empty predecessor with hashes',
    'ext4_namespace_owner_regressions': 'clean, empty and first-block ext4 images with hashes',
    'ext4_rename_relocation': 'recorded relocation attempt (--attempt)',
    'fat_fs': 'mtools and dosfstools paths (--tools)',
    'linux_dynamic_exec_x86_64': 'dynamic main and interpreter ELFs built by ObfuscationOS',
    'linux_runtime': 'static Linux ELF (--elf) built by ObfuscationOS',
    'native_object_isolation': 'toolchain root and one QEMU directory holding all ten qemu-system binaries',
    'native_process_memory_failure': 'per-architecture SDK matrix and toolchain root',
    'native_signal_ppc32': 'Linux and native PPC ELFs and an ELF corpus',
    'native_signal_production': 'production mode selection (--mode) with its SDK pair',
    'ppc_tlb': 'ppc kernel image and QEMU (--kernel, --qemu)',
    'native_firmware_handoff': 'toolchain root and one QEMU directory holding all ten qemu-system binaries',
    'native_qualified_memory': 'toolchain root at KERNEL_ROOT/toolchains with the pinned compiler identity',
    'native_runtime_frames': 'toolchain root at KERNEL_ROOT/toolchains with the pinned compiler identity',
    'jbd2': 'generated JBD2 fixture tests/fixtures/jbd2_v3.bin (scripts/generate_jbd2_fixture.py)',
    'jbd2_replay': 'generated JBD2 fixture tests/fixtures/jbd2_v3.bin (scripts/generate_jbd2_fixture.py)',
}

# Known failures on main. Each names the observed failure and its owner.
QUARANTINE = {
    'native_signal_exec': 'fatal-new/fatal-old: exec retires robust lists and clear-TID only after the '
                          'close-on-exec descriptors retire; Linux runs mm_release before do_close_on_exec',
    'native_poll_objects': 'mapping records now require their exact pool frame, so the kernel-page '
                           'admission case must be rebuilt on frame tokens',
    'native_shutdown_runtime': 'shared mode: hosted memfd ftruncate fails since shared memory files '
                               'moved onto the page pool object reserve',
    'native_child_wait_signal': 'syscall-origin capture of the hosted child-wait trap is refused',
    'native_process_signal_waits': 'syscall-origin capture of the hosted child-wait trap is refused',
    'linux_select': 'assumes console descriptors 0-2; the hosted process opens its model file as fd 0',
    'linux_udp_sockname': 'capture model: exact sockaddr copy check fails',
    'linux_udp_signal': 'captured decode model: access check fails',
    'ext4_directory_mode': 'with the generated clean image, change_mode_at("/alias/seed", 06754) is refused; '
                           'not yet compared against the Linux-mounted baseline-clean.img',
    'ext4_journalled_mode': 'with the generated clean image, change_mode_at("/alias/seed", 06754) is refused; '
                            'not yet compared against the Linux-mounted baseline-clean.img',
    'ext4_transaction': 'with the generated clean image the preview model stops at its writer report; '
                        'not yet compared against the Linux-mounted baseline-clean.img',
}

_REQUIRED = re.compile(r"add_argument\(\s*['\"](--[a-z0-9-]+)['\"][^)]*required\s*=\s*True")
_OPTION = re.compile(r"add_argument\(\s*['\"](--[a-z0-9-]+)['\"]")


def runners():
    return sorted(path.stem[5:] for path in SCRIPTS.glob('test_*.py'))


def options(name):
    text = (SCRIPTS / f'test_{name}.py').read_text()
    return set(_OPTION.findall(text)), set(_REQUIRED.findall(text))


def classify(name):
    if name in QUARANTINE:
        return 'quarantine'
    if name in TOOLCHAIN:
        return 'toolchain'
    if name in FIXTURE:
        return 'fixture'
    if name in INPUTS:
        return 'inputs'
    declared, required = options(name)
    unknown = required - {'--output'}
    if unknown:
        raise SystemExit(f'unclassified runner test_{name}.py requires {", ".join(sorted(unknown))}; '
                         'add it to hosted_suite.py')
    return 'self'


def argv(name, kind, output, toolchain_root, clean_image, sanitize):
    declared, _ = options(name)
    command = [sys.executable, '-B', str(SCRIPTS / f'test_{name}.py')]
    if kind == 'toolchain':
        command += [part.replace('{tc}', str(toolchain_root)) for part in TOOLCHAIN[name]]
    if kind == 'fixture':
        command += [part.replace('{clean_image}', str(clean_image)) for part in FIXTURE[name]]
    if '--output' in declared:
        command += ['--output', str(output / name)]
    if sanitize and '--sanitize' in declared:
        command.append('--sanitize')
    return command


def run(name, command, output, timeout):
    log = output / f'{name}.log'
    started = time.monotonic()
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    try:
        with log.open('w') as stream:
            completed = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream,
                                       stderr=subprocess.STDOUT, timeout=timeout)
        status = 'pass' if completed.returncode == 0 else f'fail({completed.returncode})'
    except subprocess.TimeoutExpired:
        status = 'timeout'
    return name, status, round(time.monotonic() - started)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, help='New directory for logs, runner output and summary.json')
    parser.add_argument('--toolchain-root', type=Path, help='SysToolchain root holding the ten <triple>/ trees')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    parser.add_argument('--shard', default='1/1', help='Run only shard I of N (I/N), by runner name')
    parser.add_argument('--only', action='append', default=[], help='Run only these runners (repeatable)')
    parser.add_argument('--ext4-fixture', action='store_true',
                        help='Build the clean ext4 image with e2fsprogs and run the runners that take it')
    parser.add_argument('--quarantined', action='store_true', help='Also run quarantined runners')
    parser.add_argument('--sanitize', action='store_true', help='Pass --sanitize to runners that take it')
    parser.add_argument('--timeout', type=int, default=3600, help='Per-runner timeout in seconds')
    parser.add_argument('--list', action='store_true', help='Print the classification and exit')
    args = parser.parse_args()

    names = runners()
    stale = sorted((set(TOOLCHAIN) | set(FIXTURE) | set(INPUTS) | set(QUARANTINE)) - set(names))
    if stale:
        raise SystemExit('hosted_suite.py names missing runners: ' + ', '.join(stale))
    kinds = {name: classify(name) for name in names}
    if args.list:
        for name in names:
            note = INPUTS.get(name) or QUARANTINE.get(name) or ''
            print(f'{kinds[name]:<10} test_{name}.py' + (f'  -- {note}' if note else ''))
        counts = {kind: sum(1 for value in kinds.values() if value == kind)
                  for kind in ('self', 'toolchain', 'fixture', 'inputs', 'quarantine')}
        print(' '.join(f'{kind}={count}' for kind, count in counts.items()))
        return 0
    if not args.output:
        parser.error('--output is required unless --list is given')
    index, _, count = args.shard.partition('/')
    if not index.isdigit() or not count.isdigit() or not 1 <= int(index) <= int(count):
        parser.error('--shard must be I/N with 1 <= I <= N')
    toolchain_root = args.toolchain_root.resolve() if args.toolchain_root else None
    if toolchain_root and not all((toolchain_root / triple / 'bin').is_dir() for triple in TRIPLES.values()):
        parser.error('--toolchain-root must hold ' + ', '.join(f'{triple}/bin' for triple in TRIPLES.values()))

    selected = []
    for name in names:
        if args.only and name not in args.only:
            continue
        if zlib.crc32(name.encode()) % int(count) != int(index) - 1:
            continue
        kind = kinds[name]
        if kind == 'self' or (kind == 'toolchain' and toolchain_root) or \
                (kind == 'fixture' and args.ext4_fixture) or (kind == 'quarantine' and args.quarantined):
            selected.append((name, kind))
    unknown = sorted(set(args.only) - set(names))
    if unknown:
        parser.error('unknown runners: ' + ', '.join(unknown))

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    clean_image = output / 'fixtures' / 'clean.img'
    if any(kind == 'fixture' for _, kind in selected):
        subprocess.run([sys.executable, '-B', str(SCRIPTS / 'make_clean_ext4_fixture.py'), '--output',
                        str(clean_image)], check=True)
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        futures = [pool.submit(run, name, argv(name, kind, output, toolchain_root, clean_image, args.sanitize),
                               output, args.timeout)
                   for name, kind in selected]
        for future in concurrent.futures.as_completed(futures):
            name, status, seconds = future.result()
            results[name] = {'kind': kinds[name], 'status': status, 'seconds': seconds}
            print(f'{status:<8} {seconds:>5}s test_{name}.py', flush=True)
    skipped = {name: kinds[name] for name in names if name not in results and
               (not args.only or name in args.only) and zlib.crc32(name.encode()) % int(count) == int(index) - 1}
    summary = {'shard': args.shard, 'toolchain_root': str(toolchain_root) if toolchain_root else None,
               'results': dict(sorted(results.items())), 'not_run': skipped}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    failed = sorted(name for name, result in results.items()
                    if result['status'] != 'pass' and result['kind'] != 'quarantine')
    passed = sum(1 for result in results.values() if result['status'] == 'pass')
    print(f'hosted suite shard {args.shard}: {passed} passed, {len(failed)} failed, {len(skipped)} not run '
          f'(inputs, quarantine, or no toolchain root or ext4 fixture); logs in {output}')
    for name in failed:
        print(f'FAILED test_{name}.py: {output / (name + ".log")}')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
