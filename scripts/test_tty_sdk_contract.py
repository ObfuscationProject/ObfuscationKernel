#!/usr/bin/env python3
"""Run the terminal SDK producer/consumer contract suites against an explicit ObfuscationOS checkout.

The suites load this checkout's tools/export_sdk.py and the consumer's
tools/kernel_sdk.py and admission fixtures, then mutate header text. They are
admission models only: no SDK export, target build, guest or hardware execution.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

SUITES = ('tests/host/native_pty_sdk_contract_tests.py', 'tests/host/console_input_sdk_contract_tests.py')
KERNEL_INPUTS = ('tools/export_sdk.py', 'include/ok/user/native_pty_profile.hpp',
                 'include/ok/sched/console_input_profile.hpp', 'include/ok/uapi/syscall.h',
                 'include/ok/syscall/syscall.hpp', *SUITES)
OS_INPUTS = ('tools/kernel_sdk.py', 'tests/tools/test_kernel_sdk.py')
SCOPE = ('Hosted SDK producer/consumer admission models over header text; no SDK export, target build, '
         'guest or hardware execution.')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--os-root', type=Path, required=True,
                        help='ObfuscationOS checkout whose tools/kernel_sdk.py consumes this SDK (read only)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    os_root = args.os_root.resolve()
    output = args.output.resolve()
    if output.is_relative_to(root) or output.is_relative_to(os_root):
        raise SystemExit('--output must be outside both checkouts')
    for name in OS_INPUTS:
        if not (os_root / name).is_file():
            raise SystemExit(f'--os-root has no {name}: {os_root}')
    output.mkdir(parents=True, exist_ok=False)
    (output / 'tmp').mkdir()
    inputs = {'kernel/' + name: sha(root / name) for name in KERNEL_INPUTS}
    inputs.update({'os/' + name: sha(os_root / name) for name in OS_INPUTS})
    revision = subprocess.run(['git', '-C', str(os_root), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    dirty = subprocess.run(['git', '--no-optional-locks', '-C', str(os_root), 'status', '--porcelain',
                            '--untracked-files=no'], capture_output=True, text=True)
    state = dict(status='running', scope=SCOPE, guest_execution=False, physical=False, smp=False,
                 kernel_root=str(root), os_root=str(os_root),
                 os_revision=revision.stdout.strip() if not revision.returncode else None,
                 os_tracked_changes=bool(dirty.stdout.strip()) if not dirty.returncode else None,
                 input_sha256=inputs, commands=[], rows=[])
    environment = dict(os.environ, OK_KERNEL_ROOT=str(root), OK_OS_ROOT=str(os_root),
                       TMPDIR=str(output / 'tmp'), PYTHONDONTWRITEBYTECODE='1')

    def save():
        temporary = output / 'result.json.tmp'
        temporary.write_text(json.dumps(state, indent=2) + '\n')
        temporary.replace(output / 'result.json')

    save()
    try:
        for suite in SUITES:
            label = Path(suite).stem
            log = output / (label + '.log')
            argv = [sys.executable, '-B', '-X', 'faulthandler', str(root / suite), '-v']
            with log.open('x') as stream:
                result = subprocess.run(argv, cwd=output, env=environment, stdout=stream,
                                        stderr=subprocess.STDOUT)
            state['commands'].append(dict(argv=argv, returncode=result.returncode, log=log.name,
                                          log_sha256=sha(log)))
            save()
            text = log.read_text()
            ran = re.search(r'^Ran (\d+) tests? in ', text, re.M)
            if result.returncode or not ran or int(ran.group(1)) == 0 or not re.search(r'^OK$', text, re.M):
                raise RuntimeError(f'{label} did not pass; see {log}')
            # A skipped consumer suite would pass without checking anything.
            if 'skipped' in text:
                raise RuntimeError(f'{label} skipped tests; see {log}')
            state['rows'].append(dict(suite=suite, status='passed', tests=int(ran.group(1))))
            save()
            print('PASS', suite, 'tests=' + ran.group(1), flush=True)
        for name, digest in inputs.items():
            base, relative = name.split('/', 1)
            if sha((root if base == 'kernel' else os_root) / relative) != digest:
                raise RuntimeError('input changed during the run: ' + name)
        state['marker'] = f'TTY_SDK_CONTRACT_PASS suites={len(SUITES)} guest=0'
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(state['marker'])


if __name__ == '__main__':
    main()
