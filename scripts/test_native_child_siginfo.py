#!/usr/bin/env python3
"""Test typed child siginfo and owned queues; no lifecycle or native execution."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
    files = [p for p in sorted((root / 'include').rglob('*')) if p.is_file()]
    files += [root / 'src/user/signals/wait_codec.hpp', root / 'src/user/signals/process_pending.hpp',
              root / 'tests/host/native_child_siginfo_tests.cpp', Path(__file__).resolve()]
    inputs = {}
    for p in files:
        name = str(p.relative_to(root))
        inputs[name] = sha(p)
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, target)
        assert sha(target) == inputs[name]
    cxx = Path(shutil.which('g++-13')).resolve()
    xmake = Path(shutil.which('xmake')).resolve()
    flags = ['-frtti', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    link = []
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
        link += ['-fsanitize=address,undefined', '-no-pie']
    quote = lambda items: ', '.join(json.dumps(x) for x in items)
    project = f'''set_project("child-siginfo-model")
set_languages("c++23")
toolchain("model-host")
set_kind("standalone")
set_toolset("cxx", {json.dumps(str(cxx))})
set_toolset("ld", {json.dumps(str(cxx))})
toolchain_end()
target("codec")
set_kind("binary")
set_toolchains("model-host")
add_includedirs("include")
add_files("tests/host/native_child_siginfo_tests.cpp")
set_targetdir("bin")
add_cxxflags({quote(flags)}, {{force=true}})
'''
    if link: project += f'add_ldflags({quote(link)}, {{force=true}})\n'
    (source / 'xmake.lua').write_text(project)
    record = dict(status='running', hosted=True, native=False, profiles=10,
                  scope='Hosted codec models for ten target wire profiles. No kernel runtime, native, SMP or physical acceptance.',
                  source_sha256=inputs, project_sha256=sha(source / 'xmake.lua'),
                  tools={str(p): sha(p) for p in (cxx, xmake)}, sanitizer=args.sanitize, commands=[])
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', XMAKE_ROOT='y',
               XMAKE_GLOBALDIR=str(out / 'xmake-global'), ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        env.pop(name, None)
    def save():
        (out / 'result.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    save()
    try:
        for label, argv in [('configure', [xmake, 'f', '-P', source, '-c', '-y', '--ccache=n']),
                            ('build', [xmake, 'build', '-P', source, '-j2', '-v']),
                            ('run', [source / 'bin/codec'])]:
            argv = list(map(str, argv))
            done = subprocess.run(argv, cwd=source, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            log = out / (label + '.log')
            log.write_bytes(done.stdout)
            record['commands'].append(dict(argv=argv, returncode=done.returncode, log=log.name, log_sha256=sha(log)))
            save()
            assert done.returncode == 0, label
        record['binary_sha256'] = sha(source / 'bin/codec')
        lines = (out / 'run.log').read_text().splitlines()
        assert len(lines) == 1 and lines[0].startswith('CHILD_SIGINFO_PASS checks=')
        assert lines[0].endswith('profiles=10 hosted=1 native=0')
        assert all(sha(source / name) == digest for name, digest in inputs.items())
        record.update(status='passed', output=lines)
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(lines[-1])


if __name__ == '__main__':
    main()
