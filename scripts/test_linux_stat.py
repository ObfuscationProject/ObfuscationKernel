#!/usr/bin/env python3
"""Retain hosted Linux stat tests separately from target/guest qualification."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def goldens(layout_path, destination):
    rows = json.loads(layout_path.read_text())['profiles']
    assert len(rows) == 10 and len({r['architecture'] for r in rows}) == 10
    destination.mkdir()
    for row in rows:
        arch, offsets = row['architecture'], row['layout']
        order = 'big' if arch in ('mips', 'mips64', 'ppc') else 'little'

        def encode(kind, values):
            data = bytearray(offsets[kind + '.size'])
            for name, value in values.items():
                prefix = kind + '.' + name
                at, width = offsets[prefix + '.offset'], offsets[prefix + '.size']
                if isinstance(value, tuple):
                    assert width == 16
                    data[at:at + 8] = value[0].to_bytes(8, order, signed=True)
                    data[at + 8:at + 12] = value[1].to_bytes(4, order)
                else:
                    data[at:at + width] = value.to_bytes(width, order, signed=value < 0)
            return data

        times = [(-1, 123456789), (2208988800, 987654321), (-2147483648, 0)]
        values = dict(stx_mask=0x7ff, stx_blksize=4096, stx_nlink=0xf1234567,
                      stx_uid=0x89abcdef, stx_gid=0xfedcba98, stx_mode=0o106754,
                      stx_ino=0xfedcba9876543210, stx_size=0x456789ab,
                      stx_blocks=0x123456789abcdef, stx_atime=times[0], stx_mtime=times[1],
                      stx_ctime=times[2], stx_dev_major=0xabc, stx_dev_minor=0xdef01)
        (destination / (arch + '-statx.bin')).write_bytes(encode('statx', values))
        if arch in ('x86_64', 'aarch64', 'rv64'):
            values = dict(st_dev=0xdefabc01, st_ino=0xfedcba9876543210, st_nlink=0xf1234567,
                          st_mode=0o106754, st_uid=0x89abcdef, st_gid=0xfedcba98,
                          st_size=0x456789ab, st_blksize=4096, st_blocks=0x123456789abcdef)
            for name, (seconds, nanos) in zip(('a', 'm', 'c'), times):
                values['st_' + name + 'time_sec'] = seconds
                values['st_' + name + 'time_nsec'] = nanos
            (destination / (arch + '-kernel.bin')).write_bytes(encode('kstat', values))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='/usr/bin/g++')
    parser.add_argument('--suite', choices=('all', 'codec', 'call', 'bridge', 'dispatch', 'blocks',
                                            'provider', 'service', 'copy-order', 'raw-blocks'), default='all')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--argument', action='append', default=[])
    args = parser.parse_args()
    if args.argument and args.suite == 'all':
        parser.error('--argument requires one explicit suite')
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    for name in ('include', 'src', 'tests/host'):
        shutil.copytree(root / name, source / name,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    (source / 'scripts').mkdir()
    runners = ('test_linux_stat.py', 'linux_stat_test_support.py',
               'test_linux_stat_blocks_oracle.py', 'test_linux_stat_provider.py',
               'test_linux_stat_service.py', 'test_linux_stat_copy_order.py')
    for name in runners:
        shutil.copy2(root / 'scripts' / name, source / 'scripts' / name)
    layouts = root / 'docs/abi/linux-stat-target-layouts.json'
    source_layouts = source / 'docs/abi/linux-stat-target-layouts.json'
    source_layouts.parent.mkdir(parents=True)
    shutil.copy2(layouts, source_layouts)
    source_hashes = {str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()}
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
    xmake = Path(shutil.which('xmake')).resolve()
    cc1 = Path(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip()).resolve()
    tools = {str(p): sha(p) for p in (compiler, xmake, cc1)}
    shared = ['src/sched/scheduler.cpp', 'src/sched/process.cpp', 'src/posix/posix.cpp',
              'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp', 'src/fs/vfs.cpp',
              'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp',
              'src/syscall/linux.cpp', 'src/arch/x86_64/ops.cpp']
    provider = ['src/fs/ext4_fs.cpp', 'tests/host/ext4_realtime_model.cpp']
    suites = {
        'codec': ['tests/host/linux_stat_codec_tests.cpp'],
        'call': ['tests/host/linux_stat_call_tests.cpp', 'src/syscall/linux.cpp'],
        'bridge': ['tests/host/linux_stat_bridge_tests.cpp'],
        'blocks': ['tests/host/ext4_blocks_tests.cpp'],
        'provider': ['tests/host/ext4_blocks_query_tests.cpp', *provider],
        'service': ['tests/host/linux_stat_service_tests.cpp', *provider, *shared],
        'copy-order': ['tests/host/linux_stat_copy_order_tests.cpp', *provider, *shared],
        'raw-blocks': ['tests/host/ext4_blocks_raw_query.cpp'],
        'dispatch': ['tests/host/linux_stat_dispatch_tests.cpp'],
    }
    for subsystem in ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
                      'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
        suites['dispatch'].extend(str(p.relative_to(source)) for p in sorted((source / 'src' / subsystem).rglob('*.cpp'))
                                  if p.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp') and p.parent.name != 'runtime')
    suites['dispatch'].extend(['src/arch/arch.cpp', 'src/arch/x86_64/ops.cpp'])
    selected = ['codec', 'call', 'bridge', 'dispatch', 'blocks'] if args.suite == 'all' else [args.suite]
    if not args.build_only and not args.argument and args.suite in ('provider', 'service', 'copy-order', 'raw-blocks'):
        parser.error('image suites require explicit --argument values or --build-only')
    project = ['set_project("linux_stat_hosted")', 'set_languages("cxx23")']
    for suite in selected:
        project.extend([f'target("stat-{suite}")', 'set_kind("binary")', 'add_includedirs("include")',
                        'add_defines("OK_ARCH_TARGET_X86_64")',
                        'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror","-ffunction-sections","-fdata-sections",{force=true})',
                        'add_ldflags("-Wl,--gc-sections",{force=true})'])
        project.extend('add_files(' + json.dumps(name) + ')' for name in suites[suite])
        if args.sanitize:
            project.extend(['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                            'add_ldflags("-fsanitize=address,undefined",{force=true})'])
        project.append('target_end()')
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    source_hashes['xmake.lua'] = sha(source / 'xmake.lua')
    state = dict(status='running', scope='Hosted common code with modeled hardware/user memory; no target or guest execution',
                 suites=selected, sanitized=args.sanitize, source_sha256=source_hashes,
                 runner_sha256=sha(Path(__file__)), tools=tools, commands=[], binaries={}, checks={})

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    def run(command, label):
        log = output / (label + '.log')
        argv = list(map(str, command))
        with log.open('x') as stream:
            result = subprocess.run(argv, cwd=source, stdout=stream, stderr=subprocess.STDOUT,
                env=dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
                         ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
        state['commands'].append(dict(argv=argv, returncode=result.returncode, log_sha256=sha(log)))
        save()
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {log}')
        return log.read_text()

    save()
    try:
        run([xmake, 'f', '-P', source, '-c', '-y', '-m', 'release', '--ccache=n', '--cxx=' + str(compiler), '--ld=' + str(compiler)], 'configure')
        run([xmake, 'build', '-P', source, '-j2', '-v'], 'build')
        for suite in selected:
            binary = source / 'build/linux/x86_64/release' / ('stat-' + suite)
            state['binaries'][suite] = dict(path=str(binary), sha256=sha(binary))
            if args.build_only:
                continue
            arguments = list(args.argument)
            if suite == 'codec':
                goldens(source_layouts, output / 'golden')
                arguments = [output / 'golden']
            text = run([binary, *arguments], suite)
            state['checks'][suite] = [int(n) for n in re.findall(r'checks=(\d+)', text)]
        for name, digest in source_hashes.items():
            assert sha(source / name) == digest, name
        for name in runners:
            assert sha(root / 'scripts' / name) == source_hashes['scripts/' + name], name
        for name, digest in tools.items():
            assert sha(name) == digest, name
        state['status'] = 'built-not-executed' if args.build_only else 'passed'
        state['artifacts'] = {str(p.relative_to(output)): sha(p) for p in sorted((output / 'golden').glob('*.bin'))}
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(state['status'], ','.join(selected), 'hosted; guest=0')


if __name__ == '__main__':
    main()
