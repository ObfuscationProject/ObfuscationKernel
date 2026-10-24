#!/usr/bin/env python3
"""Run source-pinned pending queue models; never claim hardware execution."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(exist_ok=False)
source = out / 'source'
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
record = dict(status='preparing', scope='Hosted pending queue model only',
              guest_execution=False, files={}, commands=[], tools={})
for name in ('include/ok/core/types.hpp', 'include/ok/sched/signal_mask.hpp',
             'include/ok/sched/pending_signals.hpp', 'include/ok/sched/child_exit.hpp', 'tests/host/pending_signal_tests.cpp',
             'scripts/test_pending_signals.py'):
    target = source / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / name, target)
    record['files'][name] = sha(target)
(source / 'xmake.lua').write_text('''set_project("pending-signals-model")
set_languages("c++23")
target("pending-signals-model")
set_kind("binary")
set_targetdir("artifacts")
add_files("tests/host/pending_signal_tests.cpp")
add_includedirs("include")
add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-pie",{force=true})
add_ldflags("-fsanitize=address,undefined","-no-pie",{force=true})
target_end()
''')
record['files']['xmake.lua'] = sha(source / 'xmake.lua')
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
           XMAKE_GLOBALDIR=str(out / 'xmake-global'))
record['environment'] = {key: env[key] for key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS')}
for name in ('xmake', 'g++'):
    tool = Path(shutil.which(name)).resolve()
    record['tools'][str(tool)] = sha(tool)


def save():
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')


try:
    for label, command in (
        ('configure', ['xmake', 'f', '-P', str(source), '-c', '-y', '--ccache=n']),
        ('build', ['xmake', 'build', '-P', str(source), '-j2', '-vD']),
        ('execute', [str(source / 'artifacts/pending-signals-model')]),
    ):
        with (out / (label + '.log')).open('xb') as log:
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT)
        record['commands'].append(dict(argv=command, returncode=result.returncode,
                                       log=label + '.log', sha256=sha(out / (label + '.log'))))
        save()
        if result.returncode:
            raise RuntimeError(label + ' failed; see retained log')
    for name, expected in record['files'].items():
        if sha(source / name) != expected:
            raise ValueError('retained source changed: ' + name)
    record['binary_sha256'] = sha(source / 'artifacts/pending-signals-model')
    record['status'] = 'passed-hosted-model'
except BaseException as error:
    record.update(status='failed', error=repr(error))
    save()
    raise
save()
print(out / 'result.json')
