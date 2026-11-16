#!/usr/bin/env python3
"""Retain and run the hosted job-control model: numbering, value types, the actual scheduler overlay and orphan oracle."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

SOURCES = ('tests/host/job_control_model_tests.cpp', 'src/sched/scheduler.cpp', 'src/sched/process.cpp',
           'src/memory/memory.cpp', 'src/net/native_udp.cpp', 'src/fs/vfs.cpp', 'src/arch/arch.cpp',
           'src/arch/terminal_power.cpp', 'src/arch/x86_64/ops.cpp')
MARKER = re.compile(r'^JOB_CONTROL_MODEL_PASS profiles=10 numberings=2 orphan_cases=([0-9]+) guest=0$')
# 358 group/session partitions x 3125 parent vectors x 6 groups x 6 ignored pids, plus scheduler cases.
ORACLE_CASES = 358 * 3125 * 6 * 6
SCOPE = ('Hosted model of job-control numbering, CLD_STOPPED/CLD_CONTINUED values, the actual scheduler stop '
         'overlay and the orphaned-group rule; no runtime delivery, guest, hardware isolation, physical or SMP evidence')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    record = dict(status='preparing', scope=SCOPE, hosted=True, guest=False, source_sha256={}, commands=[])

    def save():
        temporary = out / 'result.json.tmp'
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        temporary.replace(out / 'result.json')

    def run(argv, label):
        env = dict(os.environ, XMAKE_GLOBALDIR=str(out / 'xmake-global'),
                   ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(name, None)
        completed = subprocess.run(list(map(str, argv)), cwd=source, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        log = out / (label + '.log')
        log.write_bytes(completed.stdout)
        record['commands'].append(dict(argv=list(map(str, argv)), returncode=completed.returncode, log=log.name,
                                       log_sha256=sha(log)))
        save()
        if completed.returncode:
            raise RuntimeError(label + ' failed; see retained log')
        return completed.stdout.decode()

    save()
    try:
        files = {root / name for name in SOURCES}
        files.update(path for path in (root / 'include').rglob('*') if path.is_file())
        files.update((root / 'src').rglob('*.hpp'))
        files.update((root / 'src').rglob('*.inc'))
        files.add(Path(__file__).resolve())
        for path in sorted(files):
            relative = str(path.relative_to(root))
            digest = sha(path)
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            if sha(target) != digest or sha(path) != digest:
                raise ValueError('source changed while retained: ' + relative)
            record['source_sha256'][relative] = digest
        cxx = Path(shutil.which('g++') or '').resolve()
        xmake = Path(shutil.which('xmake') or '').resolve()
        if not cxx.is_file() or not xmake.is_file():
            raise ValueError('host g++ and xmake are required')
        record['tool_sha256'] = {str(path): sha(path) for path in (cxx, xmake)}
        flags = ['-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-fno-pie']
        quote = lambda items: ', '.join(json.dumps(item) for item in items)
        (source / 'xmake.lua').write_text('\n'.join([
            'set_project("job-control-model")', 'set_languages("c++23")',
            'toolchain("model-host")', '    set_kind("standalone")',
            '    set_toolset("cxx", ' + json.dumps(str(cxx)) + ')', '    set_toolset("ld", ' + json.dumps(str(cxx)) + ')',
            'toolchain_end()',
            'target("job-control-model")', '    set_kind("binary")', '    set_toolchains("model-host")',
            '    add_includedirs("include")', '    set_targetdir("bin")', '    add_defines("OK_ARCH_TARGET_X86_64")',
            '    add_cxxflags(' + quote(flags) + ', {force = true})',
            '    add_ldflags(' + quote(['-fsanitize=address,undefined', '-no-pie']) + ', {force = true})',
            '    add_files(' + quote(SOURCES) + ')', 'target_end()']) + '\n')
        record['generated_project_sha256'] = sha(source / 'xmake.lua')
        record['status'] = 'running'
        save()
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        run([xmake, 'build', '-P', source, '-j4', '-v', 'job-control-model'], 'build')
        binary = source / 'bin' / 'job-control-model'
        output = run([binary], 'execute')
        match = MARKER.match(output.rstrip().splitlines()[-1])
        if not match or int(match.group(1)) <= ORACLE_CASES:
            raise ValueError('missing job-control model marker or orphan oracle coverage')
        for relative, digest in record['source_sha256'].items():
            if sha(source / relative) != digest:
                raise ValueError('retained source changed: ' + relative)
        record.update(status='passed', binary_sha256=sha(binary), output=output.strip())
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(output.rstrip().splitlines()[-1])


if __name__ == '__main__':
    main()
