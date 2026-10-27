#!/usr/bin/env python3
"""Build focused chmod hosted models through xmake, retaining exact inputs/logs."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', choices=('all', 'dispatch', 'bridge', 'policy', 'call'), default='all')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='/usr/bin/g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    for name in ('include', 'src'):
        shutil.copytree(root / name, source / name)
    suites = ['dispatch', 'bridge', 'policy', 'call'] if args.suite == 'all' else [args.suite]
    names = ['tests/host/linux_chmod_' + suite + '_tests.cpp' for suite in suites]
    names += ['tests/host/unavailable_realtime.hpp', 'tests/host/native_runtime_units.hpp',
              'scripts/test_linux_chmod.py']
    for name in names:
        (source / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, source / name)
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
    xmake = Path(shutil.which('xmake')).resolve()
    cc1 = Path(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip()).resolve()
    tools = {str(p): sha(p) for p in (compiler, xmake, cc1)}
    shared = []
    if 'dispatch' in suites:
        for subsystem in ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc',
                          'memory', 'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
            shared += [str(p.relative_to(source)) for p in sorted((source / 'src' / subsystem).rglob('*.cpp'))
                       if p.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp') and p.parent.name != 'runtime']
        shared += ['src/arch/arch.cpp', 'src/arch/x86_64/ops.cpp']
    project = ['set_project("linux_chmod_hosted")', 'set_languages("cxx23")']
    for suite in suites:
        project += [f'target("chmod-{suite}")', 'set_kind("binary")',
                    'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
                    'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror",'
                    '"-ffunction-sections","-fdata-sections",{force=true})',
                    'add_ldflags("-Wl,--gc-sections",{force=true})']
        files = ['tests/host/linux_chmod_' + suite + '_tests.cpp']
        if suite == 'dispatch':
            files += shared
        elif suite == 'call':
            files += ['src/syscall/linux.cpp']
        project += ['add_files(' + json.dumps(name) + ')' for name in files]
        if args.sanitize:
            project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                        'add_ldflags("-fsanitize=address,undefined",{force=true})']
        project += ['target_end()']
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    sources = {str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()}
    state = dict(status='running', scope='Hosted policy/call models and actual dispatcher/service; injected provider outcomes and modeled user memory. No ext4 qualification, target or guest execution.',
                 suites=suites, sanitized=args.sanitize, source_sha256=sources, tools=tools,
                 commands=[], binaries={}, checks={}, qemu=False)

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    def run(command, label):
        log = output / (label + '.log')
        argv = list(map(str, command))
        with log.open('x') as stream:
            result = subprocess.run(argv, cwd=source, stdout=stream, stderr=subprocess.STDOUT,
                env=dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
                         ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
        state['commands'].append(dict(argv=argv, returncode=result.returncode, log=str(log), log_sha256=sha(log)))
        save()
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {log}')
        return log.read_text()

    save()
    try:
        run([xmake, 'f', '-P', source, '-c', '-y', '-m', 'release', '--ccache=n',
             '--cxx=' + str(compiler), '--ld=' + str(compiler)], 'configure')
        run([xmake, 'build', '-P', source, '-j2', '-v'], 'build')
        for suite in suites:
            binary = source / 'build/linux/x86_64/release' / ('chmod-' + suite)
            state['binaries'][suite] = dict(path=str(binary), sha256=sha(binary))
            text = run([binary], suite)
            state['checks'][suite] = [int(n) for n in re.findall(r'checks=(\d+)', text)]
            if suite == 'dispatch':
                assert len(re.findall(r'^PROFILE ', text, re.M)) == 10
            assert 'PASS ' in text and 'guest=0' in text
        for name, digest in sources.items():
            assert sha(source / name) == digest, name
        for name, digest in tools.items():
            assert sha(name) == digest, name
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print('PASS', ','.join(suites), 'hosted; guest=0')


if __name__ == '__main__':
    main()
