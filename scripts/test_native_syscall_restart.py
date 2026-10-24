#!/usr/bin/env python3
"""Build and execute all-ten pure restart adapters with a hosted xmake target."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    record = dict(status='preparing', scope='Pure hosted adapter: no owned capture, runtime publication or guest execution',
                  files={}, commands=[], profiles=10, guest=False)
    files = sorted((root / 'include').rglob('*.hpp')) + [
        root / 'tests/host/native_syscall_restart_tests.cpp', Path(__file__).resolve()]
    for path in files:
        target = source / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        record['files'][str(path.relative_to(root))] = sha(path)
        assert sha(target) == sha(path)
    xmake = Path(shutil.which('xmake')).resolve()
    cxx = Path(shutil.which('g++')).resolve()
    record['tools'] = {str(p): sha(p) for p in (xmake, cxx)}
    (source / 'xmake.lua').write_text('''set_project("native-restart-adapter-hosted")
set_languages("c++23")
toolchain("restart-host")
set_kind("standalone")
set_toolset("cxx", CXX)
set_toolset("ld", CXX)
toolchain_end()
target("restart-adapter")
set_kind("binary")
set_toolchains("restart-host")
set_targetdir("artifacts")
add_files("tests/host/native_syscall_restart_tests.cpp")
add_includedirs("include")
add_defines("OK_ARCH_TARGET_X86_64")
add_cxxflags("-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", {force=true})
add_ldflags("-fsanitize=address,undefined", "-no-pie", {force=true})
target_end()
'''.replace('CXX', json.dumps(str(cxx))))
    record['project_sha256'] = sha(source / 'xmake.lua')
    env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out / 'xmake-global'),
               PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        env.pop(name, None)
    def save():
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    def run(argv, label):
        argv = list(map(str, argv))
        with (out / (label + '.log')).open('wb') as log:
            result = subprocess.run(argv, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT)
        record['commands'].append(dict(argv=argv, returncode=result.returncode, log=label + '.log',
                                       log_sha256=sha(out / (label + '.log'))))
        save()
        if result.returncode:
            raise RuntimeError(label + ' failed')
    save()
    try:
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        run([xmake, 'build', '-P', source, '-j2', '-v', 'restart-adapter'], 'build')
        binary = source / 'artifacts/restart-adapter'
        run([binary], 'execute')
        output = (out / 'execute.log').read_text().strip()
        assert output.startswith('RESTART_ADAPTER_PASS profiles=10 variants=44 checks=')
        assert output.endswith(' hosted=1 guest=0')
        for name, value in record['files'].items():
            assert sha(source / name) == value
        record.update(status='passed', binary_sha256=sha(binary), output=output)
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(record['output'])


if __name__ == '__main__':
    main()
