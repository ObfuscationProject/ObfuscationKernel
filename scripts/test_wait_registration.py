#!/usr/bin/env python3
"""Retain xmake-built wait ownership model checks; no guest or SMP execution."""
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
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='/usr/bin/g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    names = ('include/ok/core/types.hpp', 'include/ok/sched/wait_registration.hpp',
             'tests/kernel/points/wait_registration_checks.hpp',
             'tests/host/wait_registration_tests.cpp', 'scripts/test_wait_registration.py')
    for name in names:
        destination = source / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, destination)
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
    xmake_name = shutil.which('xmake')
    if not xmake_name:
        raise RuntimeError('xmake is required')
    xmake = Path(xmake_name).resolve()
    project = [
        'set_project("wait-registration-hosted")', 'set_languages("cxx23")',
        'target("wait-registration")', 'set_kind("binary")', 'set_targetdir("artifacts")',
        'add_includedirs("include")', 'add_files("tests/host/wait_registration_tests.cpp")',
        'add_cxxflags("-O1", "-g", "-frtti", "-fno-exceptions", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force=true})',
    ]
    if args.sanitize:
        project += ['add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force=true})',
                    'add_ldflags("-fsanitize=address,undefined", {force=true})']
    project += ['target_end()']
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    source_hashes = {name: sha(source / name) for name in (*names, 'xmake.lua')}
    state = dict(status='running', scope='hosted wait ownership model', guest_execution=False,
                 smp=False, physical=False, sanitizer=args.sanitize, leak_sanitizer=False,
                 source_sha256=source_hashes, tools={str(p): sha(p) for p in (compiler, xmake)},
                 commands=[])
    environment = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
                       ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    def run(argv, label):
        argv = list(map(str, argv))
        log = output / (label + '.log')
        with log.open('x') as stream:
            result = subprocess.run(argv, cwd=source, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        state['commands'].append(dict(argv=argv, returncode=result.returncode, log=str(log), sha256=sha(log)))
        save()
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {log}')
        return log.read_text()

    save()
    try:
        run([xmake, 'f', '-P', source, '-c', '-y', '-m', 'release', '--ccache=n',
             '--cxx=' + str(compiler), '--ld=' + str(compiler)], 'configure')
        run([xmake, 'build', '-P', source, '-j2', '-v'], 'build')
        binary = source / 'artifacts/wait-registration'
        state['binary'] = dict(path=str(binary), sha256=sha(binary))
        text = run([binary], 'execute')
        match = re.fullmatch(r'PASS wait-registration checks=(\d+) scope=hosted-model guest=0 smp=0\n', text)
        if not match:
            raise RuntimeError('missing exact model completion marker')
        state['checks'] = int(match.group(1))
        for name, digest in source_hashes.items():
            if sha(source / name) != digest:
                raise RuntimeError('retained source changed: ' + name)
        for name in names:
            if sha(root / name) != source_hashes[name]:
                raise RuntimeError('live source changed during model run: ' + name)
        for name, digest in state['tools'].items():
            if sha(name) != digest:
                raise RuntimeError('tool changed: ' + name)
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(f"PASS wait-registration checks={state['checks']} hosted; guest=0 smp=0")


if __name__ == '__main__':
    main()
