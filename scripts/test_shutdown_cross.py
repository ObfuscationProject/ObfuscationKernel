#!/usr/bin/env python3
"""Compile changed shutdown mechanisms for all ten Debug/Release profiles."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
from test_ext4_owned import PROFILES, TARGET_FLAGS


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    parser.add_argument('--debug-point-only', action='store_true')
    parser.add_argument('--native-integration', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    paths = ['src/core/kernel.cpp', 'src/core/kernel_main.cpp', 'src/fs/vfs.cpp',
             'tests/host/shutdown_debug_compile.cpp', 'tests/host/shutdown_coordinator_compile.cpp',
             'tests/kernel/points/test_point.cpp']
    if args.native_integration:
        paths += ['src/core/native_shutdown.cpp', 'src/core/entry.cpp', 'src/core/module.cpp',
                  'src/driver/driver.cpp', 'src/sched/scheduler.cpp', 'src/user/runtime.cpp',
                  'src/arch/terminal_power.cpp']
    if args.debug_point_only: paths = ['tests/kernel/points/test_point.cpp']
    files = {root / name for name in paths} | {Path(__file__).resolve(), root / 'scripts/test_ext4_owned.py'}
    if args.native_integration:
        files.update((root / 'src/arch').glob('*/execution.cpp'))
    for base in ('include', 'src', 'tests'):
        files.update(p for p in (root / base).rglob('*') if p.is_file() and p.suffix in ('.hpp', '.h', '.inc'))
    report = dict(status='preparing', scope='All-ten freestanding compile only; no guest or physical execution.',
                  commands=[], rows=[], source_sha256={}, warning_policy='Production units retain production warning flags; new compile instantiations additionally require -Werror.')
    def save():
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    def run(argv, label):
        env = dict(os.environ, XMAKE_ROOT='y', XMAKE_TMPDIR=str(out / 'xmake-tmp'))
        command = list(map(str, argv))
        result = subprocess.run(command, cwd=source, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = out / (label + '.log'); log.write_bytes(result.stdout)
        report['commands'].append(dict(argv=command, returncode=result.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if result.returncode: raise RuntimeError(label + ' failed; inspect ' + str(log))
    try:
        for path in sorted(files):
            name = str(path.relative_to(root)); target = source / name
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
            report['source_sha256'][name] = sha(target)
            if sha(path) != sha(target): raise ValueError('snapshot changed: ' + name)
        flags = ['-ffreestanding', '-fno-exceptions', '-fno-stack-protector', '-fno-use-cxa-atexit',
                 '-fno-threadsafe-statics', '-frtti', '-O2', '-Wall', '-Wextra', '-Wpedantic']
        lua = ['set_project("shutdown-cross")', 'set_languages("c++23")']
        report['tools'] = {}
        for arch, triple, *_ in PROFILES:
            prefix = args.toolchain_root.resolve() / triple / 'bin' / (triple + '-')
            cxx, ar = Path(str(prefix) + 'g++'), Path(str(prefix) + 'ar')
            if not cxx.is_file() or not ar.is_file(): raise ValueError('missing explicit toolchain: ' + arch)
            report['tools'][arch] = dict(compiler=str(cxx), compiler_sha256=sha(cxx), ar=str(ar), ar_sha256=sha(ar))
            arch_flags = TARGET_FLAGS[arch] + (['-msimd=none'] if arch == 'loongarch64' else [])
            lua += ['toolchain("cross-' + arch + '")', 'set_kind("standalone")',
                    'set_toolset("cxx", ' + json.dumps(str(cxx)) + ')', 'set_toolset("ar", ' + json.dumps(str(ar)) + ')',
                    'toolchain_end()']
            for mode in ('debug', 'release'):
                definitions = ['OK_ARCH_TARGET_' + arch.upper(), 'OK_NATIVE_SIGNAL_RUNTIME']
                selected_paths = list(paths)
                if args.native_integration:
                    definitions += ['OK_KERNEL_FREESTANDING']
                    selected_paths += ['src/arch/' + arch + '/execution.cpp']
                if mode == 'debug': definitions += ['OK_ENABLE_TEST_POINTS']
                mode_flags = [f for f in flags if f != '-O2'] + (['-O2', '-g'] if mode == 'debug' else ['-O2'])
                lua += ['target("shutdown-' + arch + '-' + mode + '")', 'set_kind("static")',
                        'set_toolchains("cross-' + arch + '")', 'add_includedirs("include")',
                        'set_targetdir("lib")', 'add_defines(' + ', '.join(map(json.dumps, definitions)) + ')',
                        'add_cxxflags(' + ', '.join(json.dumps(f) for f in mode_flags + arch_flags) + ', {force=true})',
                        'add_files(' + ', '.join(json.dumps(p) for p in selected_paths if not p.startswith('tests/host/shutdown_')) + ')',
                        *['add_files(' + json.dumps(p) + ', {cxxflags="-Werror"})' for p in selected_paths if p.startswith('tests/host/shutdown_')], 'target_end()']
        (source / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        report['generated_project_sha256'] = sha(source / 'xmake.lua'); report['status'] = 'running'; save()
        run(['xmake', 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        run(['xmake', 'build', '-P', source, '-j4', '-v'], 'all-ten-build')
        for arch, _, elf_class, endian, machine in PROFILES:
            for mode in ('debug', 'release'):
                artifact = source / 'lib' / ('libshutdown-' + arch + '-' + mode + '.a')
                data = artifact.read_bytes(); offset = 8; identities = []
                if data[:8] != b'!<arch>\n': raise ValueError('invalid cross archive: ' + arch)
                while offset < len(data):
                    header = data[offset:offset + 60]
                    if len(header) != 60 or header[58:] != b'`\n': raise ValueError('invalid archive header: ' + arch)
                    size = int(header[48:58]); body = data[offset + 60:offset + 60 + size]
                    if body[:4] == b'\x7fELF': identities.append([body[4], body[5], int.from_bytes(body[18:20], 'little' if body[5] == 1 else 'big')])
                    offset += 60 + size + size % 2
                if offset != len(data) or identities != [[elf_class, endian, machine]] * (len(paths) + int(args.native_integration)): raise ValueError('cross object identity/count mismatch: ' + arch)
                report['rows'].append(dict(arch=arch, mode=mode, status='passed', archive_sha256=sha(artifact), elf_identities=identities))
                save(); print('PASS freestanding shutdown ' + arch + ' ' + mode + ' (not executed)', flush=True)
        for name, digest in report['source_sha256'].items():
            if sha(source / name) != digest: raise ValueError('retained source changed: ' + name)
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=repr(error)); save(); raise
    save()


if __name__ == '__main__':
    main()
