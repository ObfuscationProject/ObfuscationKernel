#!/usr/bin/env python3
"""Build the actual native metadata dispatcher helper through xmake with retained inputs/logs."""
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
    for name in ('include', 'src'):
        shutil.copytree(root / name, source / name)
    names = ['tests/host/native_metadata_tests.cpp', 'scripts/test_native_metadata.py']
    for name in names:
        (source / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, source / name)
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
    xmake = Path(shutil.which('xmake')).resolve()
    cc1 = Path(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip()).resolve()
    tools = {str(p): sha(p) for p in (compiler, xmake, cc1)}
    project = ['set_project("native_metadata_hosted")', 'set_languages("cxx23")',
        'target("native-metadata")', 'set_kind("binary")', 'add_includedirs("include")',
        'add_defines("OK_ARCH_TARGET_X86_64")',
        'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror",'
        '"-ffunction-sections","-fdata-sections",{force=true})',
        'add_ldflags("-Wl,--gc-sections",{force=true})',
        'add_files("tests/host/native_metadata_tests.cpp", "src/syscall/linux.cpp")']
    if args.sanitize:
        project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                    'add_ldflags("-fsanitize=address,undefined",{force=true})']
    project += ['target_end()']
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    sources = {str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()}
    state = dict(status='running', scope='Actual native metadata dispatch helper and errno mapper; modeled user memory, injected checked service outcomes, 32/64-bit word models. No provider, target or guest execution.', sanitized=args.sanitize, source_sha256=sources, tools=tools,
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
        binary = source / 'build/linux/x86_64/release/native-metadata'
        state['binaries']['dispatch'] = dict(path=str(binary), sha256=sha(binary))
        output_text = run([binary], 'dispatch')
        state['checks']['dispatch'] = [int(n) for n in re.findall(r'checks=(\d+)', output_text)]
        assert 'PASS NATIVE_METADATA_DISPATCH' in output_text and 'guest=0' in output_text
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
    print('PASS native metadata dispatch hosted; guest=0')


if __name__ == '__main__':
    main()
