#!/usr/bin/env python3
"""Build and run every target of tests/host/native_pty/xmake.lua from a retained source snapshot.

The suites exercise the raw PTY transport, its descriptor owners and the actual
hosted scheduler/runtime on mock trap/copy backends. Passing claims hosted model
evidence only; no guest, hardware isolation, physical or SMP execution.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import resource
import shutil
import subprocess
from hosted_closure import snapshot_members

PROJECT = 'tests/host/native_pty'
MODE = 'debug'
# Every target of the project with the exact completion line its binary must print.
# Static libraries and the freestanding boot-runner compile check only build.
BINARIES = {
    'native-pty-ofd-tests': r'PRIVATE_PTY_ACTUAL_OFD cases=\d+ checks=\d+ status=passed',
    'native-pty-raw-owner-tests':
        r'PRIVATE_RAW_PTY_OWNER cases=\d+ checks=\d+ model_steps=100000 status=passed',
    'native-pty-pair-dispatch-tests':
        r'NATIVE_PTY_PAIR_BOUNDARY_PASS profiles=\d+ checks=\d+ hosted_template_only=1',
    'native-pty-boot-checks-tests':
        r'NATIVE_PTY_BOOT_MODEL_HOSTED_PASS iterations=128 unrelated_owner_preserved=1 guest=0',
    'native-pty-unix-rights-regressions': r'UNIX_RIGHTS_HOSTED_PASS checks=\d+',
    'native-pty-unix-path-regressions': r'UNIX_PATH_OWNER_HOSTED_PASS checks=\d+',
    'native-pty-shared-rights-regressions':
        r'PASS shared file UNIX rights ownership: checks=\d+ profiles=10 native_codec=1 hosted=1 '
        r'target_PTE=0 guest=0',
    'native-pty-runtime-tests':
        r'PTY_RUNTIME_PASS actual_runtime=1 hosted=1 guest=0 cases=\d+ checks=\d+ allocations=\d+',
}
BUILD_ONLY = ('native-pty-owner-hosted-runtime', 'native-pty-actual-runtime-hosted-library',
              'native-pty-boot-runner-compile')
SCOPE = ('Hosted actual scheduler/runtime model and raw PTY owner/transport checks on the x86_64 host '
         'profile, with mock trap/copy/mapping backends; no guest, hardware isolation, physical or SMP '
         'evidence.')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.is_relative_to(root):
        raise SystemExit('--output must be outside the source tree')
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source'
    project = source / PROJECT
    build = output / 'build'
    state = dict(status='preparing', scope=SCOPE, guest_execution=False, physical=False, smp=False,
                 sanitizer=args.sanitize, leak_detection=False, mode=MODE, runtime_tests=True,
                 source_sha256={}, commands=[], rows=[])
    environment = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
                       XMAKE_TMPDIR=str(output / 'xmake-tmp'),
                       ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX',
                 'COMPILER_PATH'):
        environment.pop(name, None)

    def save():
        temporary = output / 'result.json.tmp'
        temporary.write_text(json.dumps(state, indent=2) + '\n')
        temporary.replace(output / 'result.json')

    def run(argv, label):
        argv = list(map(str, argv))
        log = output / (label + '.log')
        with log.open('x') as stream:
            result = subprocess.run(argv, cwd=project, env=environment, stdout=stream,
                                    stderr=subprocess.STDOUT)
        state['commands'].append(dict(argv=argv, returncode=result.returncode, log=log.name,
                                      log_sha256=sha(log)))
        save()
        text = log.read_text(errors='replace')
        if 'You are working in the project directory' in text:
            raise RuntimeError(f'{label} resolved an enclosing project; see {log}')
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {log}')
        return text

    save()
    try:
        # The runtime library compiles most of src/, so the snapshot is the whole
        # source tree rather than an include closure.
        for path in snapshot_members(root):
            name = path.relative_to(root).as_posix()
            digest = sha(path)
            destination = source / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            if sha(destination) != digest:
                raise ValueError('snapshot changed: ' + name)
            state['source_sha256'][name] = digest
        state['runner_sha256'] = sha(__file__)
        compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
        xmake_name = shutil.which('xmake')
        if not compiler.is_file() or not xmake_name:
            raise ValueError('xmake and compiler required')
        xmake = Path(xmake_name).resolve()
        state['tool_sha256'] = {str(p): sha(p) for p in (compiler, xmake)}
        state['status'] = 'running'
        save()
        run([xmake, 'f', '-P', project, '-c', '-y', '-p', 'linux', '-a', 'x86_64', '-m', MODE,
             '-o', build, '--ccache=n', '--cxx=' + str(compiler), '--ld=' + str(compiler),
             '--ar=ar', '--runtime_tests=y', '--sanitizers=' + ('y' if args.sanitize else 'n')],
            'configure')
        listed = run([xmake, 'show', '-P', project, '-l', 'targets'], 'targets')
        declared = set(re.findall(r'native-pty-[a-z0-9-]+', listed))
        expected = set(BINARIES) | set(BUILD_ONLY)
        if declared != expected:
            raise ValueError('project targets differ from the runner table: ' +
                             ', '.join(sorted(declared ^ expected)))
        run([xmake, 'build', '-P', project, '-a', '-j4', '-v'], 'build')
        for name in BUILD_ONLY:
            state['rows'].append(dict(target=name, status='built'))
        for name, marker in BINARIES.items():
            binary = build / 'linux' / 'x86_64' / MODE / name
            if not binary.is_file():
                raise ValueError('missing built binary: ' + str(binary))
            text = run([binary], name)
            lines = re.findall('^' + marker + '$', text, re.M)
            if len(lines) != 1:
                raise ValueError('missing exact completion marker: ' + name)
            state['rows'].append(dict(target=name, status='passed', binary_sha256=sha(binary),
                                      marker=lines[0]))
            save()
            print(name + ':', lines[0], flush=True)
        for name, digest in state['source_sha256'].items():
            if sha(source / name) != digest:
                raise ValueError('retained source changed: ' + name)
        for name, digest in state['tool_sha256'].items():
            if sha(name) != digest:
                raise ValueError('tool changed: ' + name)
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(f'PASS native PTY suites targets={len(BINARIES) + len(BUILD_ONLY)} hosted=1 guest=0')


if __name__ == '__main__':
    main()
