#!/usr/bin/env python3
"""Check actual native page ownership using hosted backing; no mapping/hardware qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(output, sanitize, cxx):
    root = Path(__file__).resolve().parents[1]
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    names = ['src/memory/memory.cpp', 'tests/host/native_page_pool_tests.cpp', 'scripts/test_native_page_pool.py']
    names += [str(p.relative_to(root)) for p in sorted((root / 'include').rglob('*')) if p.is_file()]
    for name in names:
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, target)
    compiler = Path(shutil.which(cxx) or cxx).resolve()
    xmake = Path(shutil.which('xmake')).resolve()
    cc1 = Path(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip()).resolve()
    project = ['set_project("native-page-provider")', 'set_languages("cxx23")',
               'target("native-page-provider")', 'set_kind("binary")', 'set_targetdir("artifacts")',
               'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
               'add_files("tests/host/native_page_pool_tests.cpp", "src/memory/memory.cpp")',
               'add_cxxflags("-O1", "-g", "-frtti", "-fno-exceptions", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-ffunction-sections", "-fdata-sections", {force=true})',
               'add_ldflags("-Wl,--gc-sections", {force=true})']
    if sanitize:
        project += ['add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force=true})',
                    'add_ldflags("-fsanitize=address,undefined", {force=true})']
    (source / 'xmake.lua').write_text('\n'.join(project + ['target_end()']) + '\n')
    inputs = {name: sha(source / name) for name in [*names, 'xmake.lua']}
    result = dict(status='running', scope='Hosted actual native ownership provider over aligned local backing; no PTE, firmware, guest, physical or SMP qualification',
                  source_sha256=inputs, tools={str(p): sha(p) for p in (compiler, xmake, cc1)},
                  sanitizer=sanitize, leak_detection=False, commands=[])
    environment = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
                       ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        environment.pop(name, None)

    def save():
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')

    def command(argv, label):
        argv = list(map(str, argv))
        log = output / (label + '.log')
        with log.open('x') as stream:
            done = subprocess.run(argv, cwd=source, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        result['commands'].append(dict(argv=argv, returncode=done.returncode, log_sha256=sha(log)))
        save()
        if done.returncode:
            raise RuntimeError(str(log))
        return log.read_text()

    save()
    try:
        command([xmake, 'f', '-P', source, '-c', '-y', '-m', 'release', '--ccache=n',
                 '--cxx=' + str(compiler), '--ld=' + str(compiler)], 'configure')
        command([xmake, 'build', '-P', source, '-j2', '-v'], 'build')
        binary = source / 'artifacts/native-page-provider'
        result['binary'] = dict(path=str(binary), sha256=sha(binary))
        marker = command([binary], 'execute')
        if not marker.startswith('PASS native page provider:') or not marker.endswith('hosted=1 guest=0 smp=0\n'):
            raise RuntimeError('missing native page provider completion marker')
        for name, digest in inputs.items():
            if sha(source / name) != digest:
                raise RuntimeError('retained source changed: ' + name)
        for name, digest in result['tools'].items():
            if sha(name) != digest:
                raise RuntimeError('tool changed: ' + name)
        result['status'] = 'passed'
    except BaseException as error:
        result.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print(marker.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    if args.output:
        run(args.output.resolve(), args.sanitize, args.cxx)
    else:
        with tempfile.TemporaryDirectory(prefix='ok-native-pool-') as directory:
            run(Path(directory) / 'run', args.sanitize, args.cxx)


if __name__ == '__main__':
    main()
