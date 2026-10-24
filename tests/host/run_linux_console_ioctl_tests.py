#!/usr/bin/env python3
"""Build and run copied-byte Linux console codec models using xmake."""
from datetime import datetime, timezone
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def identity(path):
    data = Path(path).read_bytes()
    return dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--xmake', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(mode=0o700)
    project = output / 'project'
    project.mkdir(mode=0o700)
    tests = ('linux_console_ioctl_tests', 'linux_user_abi_tests',
             'linux_stat_codec_tests', 'linux_chmod_call_tests')
    sources = ['include/ok/abi/linux_console_ioctl.hpp', 'include/ok/abi/linux_profile_data.hpp',
               'include/ok/abi/linux_user.hpp', 'scripts/generate_linux_abi.py',
               'docs/abi/linux-profiles.json', 'tests/host/linux_abi_generator_tests.py',
               'src/syscall/linux.cpp', 'include/ok/core/types.hpp',
               'scripts/test_linux_stat.py', 'docs/abi/linux-stat-target-layouts.json',
               'tests/host/run_linux_console_ioctl_tests.py'] + ['tests/host/' + name + '.cpp' for name in tests]
    pins = {str(ROOT / name): identity(ROOT / name) for name in sources}
    lines = ['set_project("linux-console-ioctl-host-models")', 'set_languages("c++23")']
    for name in tests:
        lines += ['target(' + json.dumps(name) + ')', '    set_kind("binary")',
                  '    set_targetdir(' + json.dumps(str(output / 'bin')) + ')',
                  '    add_includedirs(' + json.dumps(str(ROOT / 'include')) + ')',
                  '    add_files(' + json.dumps(str(ROOT / 'tests/host' / (name + '.cpp'))) + ')',
                  '    add_cxxflags("-Wall", "-Wextra", "-Werror")']
        if name == 'linux_chmod_call_tests':
            lines += ['    add_files(' + json.dumps(str(ROOT / 'src/syscall/linux.cpp')) + ')',
                      '    add_cxxflags("-ffunction-sections", "-fdata-sections")',
                      '    add_ldflags("-Wl,--gc-sections")']
        if args.sanitize:
            lines += ['    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")',
                      '    add_ldflags("-fsanitize=address,undefined")']
    (project / 'xmake.lua').write_text('\n'.join(lines) + '\n')
    environment = dict(os.environ, XMAKE_ROOT='y', PYTHONDONTWRITEBYTECODE='1', LC_ALL='C', TZ='UTC',
                       XMAKE_GLOBALDIR=str(output / 'xmake-global'), XMAKE_TMPDIR=str(output / 'xmake-temp'),
                       ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    record = dict(schema=1, status='running', started_utc=datetime.now(timezone.utc).isoformat(),
                  scope='Hosted copied-byte codec models only; no user pointer access, kernel execution, guest, hardware or publication.',
                  sanitize=args.sanitize, inputs=pins, commands=[])
    def save():
        (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    def run(label, argv, cwd):
        log = output / (label + '.log')
        row = dict(label=label, argv=list(map(str, argv)), cwd=str(cwd), log=str(log))
        record['commands'].append(row)
        save()
        with log.open('xb') as stream:
            result = subprocess.run(row['argv'], cwd=cwd, env=environment, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=240)
        row.update(returncode=result.returncode, log_identity=identity(log))
        save()
        print(label, result.returncode, flush=True)
        if result.returncode:
            raise RuntimeError('host codec command failed: ' + label)
    try:
        run('generator-contracts', [sys.executable, '-B', '-m', 'unittest', 'discover', '-s',
                                  'tests/host', '-p', 'linux_abi_generator_tests.py'], ROOT)
        run('configure', [args.xmake, 'f', '-c', '-y', '-p', 'linux', '-a', 'x86_64', '-m', 'debug',
                          '--toolchain=gcc', '--ccache=n'], project)
        run('build', [args.xmake, 'build', '-y', '-v', '-j3'], project)
        spec = importlib.util.spec_from_file_location('console_ioctl_stat_regression_support',
                                                    ROOT / 'scripts/test_linux_stat.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        support.goldens(ROOT / 'docs/abi/linux-stat-target-layouts.json', output / 'stat-golden')
        for name in tests:
            run(name, [output / 'bin' / name, *([output / 'stat-golden'] if name == 'linux_stat_codec_tests' else [])], project)
        if any(identity(path) != expected for path, expected in pins.items()):
            raise RuntimeError('codec input changed during test')
        record.update(status='passed', source_inputs_unchanged=True)
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        raise
    finally:
        record['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
