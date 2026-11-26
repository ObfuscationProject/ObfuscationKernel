#!/usr/bin/env python3
"""Hosted TCP/UDP socket runtime tests against the actual native runtime.

Each test compiles the runtime units as one translation unit
(tests/host/native_runtime_units.hpp) over mock trap, copy and mapping
backends and links the rest of the kernel sources; the extension's NIC is a
model peer. No guest, NIC, hardware isolation or SMP is qualified here.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

TESTS = (
    'tcp_immediate_runtime_tests',
    'tcp_accept_transaction_tests',
    'native_udp_receive_tests',
    'linux_udp_operation_body_tests',
    'linux_udp_receive_order_tests',
    'linux_udp_address_tests',
    'native_tcp_shared_link_tests',
    'linux_udp_connected_capture_tests',
    'linux_udp_connected_runtime_tests',
    'linux_udp_dispatch_integration_tests',
    'linux_udp_owner_store_tests',
    'linux_udp_receive_ownership_tests',
    'linux_udp_restart_capture_tests',
    'linux_udp_restart_runtime_tests',
    'linux_udp_runtime_dispatch_tests',
    'linux_udp_signal_runtime_tests',
    'linux_udp_sockname_capture_tests',
    'linux_udp_sockname_runtime_tests',
)
sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='/usr/bin/g++')
    parser.add_argument('--only', action='append', choices=TESTS)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    for name in ('include', 'src', 'tests'):
        shutil.copytree(root / name, source / name)
    tests = args.only or list(TESTS)
    shared = []
    for subsystem in ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc',
                      'memory', 'net', 'posix', 'sched', 'smp', 'syscall', 'user'):
        shared += [str(p.relative_to(source)) for p in sorted((source / 'src' / subsystem).rglob('*.cpp'))
                   if p.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp') and p.parent.name != 'runtime']
    shared += ['src/arch/arch.cpp', 'src/arch/terminal_power.cpp', 'src/arch/x86_64/ops.cpp']
    flags = ['-O1', '-g', '-frtti', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-ffunction-sections', '-fdata-sections']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    common = ['add_includedirs("include", "tests/host")', 'add_defines("OK_ARCH_TARGET_X86_64")',
              'add_cxxflags(' + ','.join(map(json.dumps, flags)) + ',{force=true})']
    project = ['set_project("net_runtime_hosted")', 'set_languages("cxx23")',
               'target("kernel-closure")', 'set_kind("static")', *common]
    project += ['add_files(' + json.dumps(name) + ')' for name in shared]
    project += ['target_end()']
    for test in tests:
        project += [f'target({json.dumps(test)})', 'set_kind("binary")', 'add_deps("kernel-closure")', *common,
                    'add_files(' + json.dumps('tests/host/' + test + '.cpp') + ')',
                    'add_ldflags("-Wl,--gc-sections"' + (',"-fsanitize=address,undefined"' if args.sanitize else '') +
                    ',{force=true})', 'target_end()']
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    result = dict(status='running', scope='hosted model: actual runtime units, mock backends, model NIC peer',
                  tests=tests, sanitized=args.sanitize, script_sha256=sha(__file__), runs={})
    save = lambda: (output / 'result.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    env = dict(os.environ, XMAKE_GLOBALDIR=str(output / 'xmake-global'), ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
    def run(argv, label):
        log = output / (label + '.log')
        with log.open('wb') as stream:
            done = subprocess.run(list(map(str, argv)), cwd=source, env=env, stdout=stream, stderr=subprocess.STDOUT)
        return done.returncode, log
    save()
    xmake = shutil.which('xmake')
    code, log = run([xmake, 'f', '-c', '-y', '--ccache=n', '--cxx=' + args.cxx, '--ld=' + args.cxx], 'configure')
    if code:
        result.update(status='failed', failed='configure'); save()
        sys.exit(f'configure failed: {log}')
    failed = []
    for test in tests:
        # One target at a time, so a test that does not build is reported alone.
        code, log = run([xmake, 'build', '-j8', test], 'build-' + test)
        binaries = [p for p in (source / 'build').rglob(test) if p.is_file() and os.access(p, os.X_OK)] if not code else []
        if not binaries:
            result['runs'][test] = dict(returncode=None, last_line='build failed: ' + log.name)
            print('FAIL ' + test + ': build failed', flush=True); failed.append(test); continue
        binary = binaries[0]
        code, log = run([binary], 'run-' + test)
        tail = log.read_text(errors='replace').strip().splitlines()[-1:] or ['']
        result['runs'][test] = dict(returncode=code, last_line=tail[0])
        print(('PASS ' if not code else 'FAIL ') + test + ': ' + tail[0], flush=True)
        if code: failed.append(test)
    result['status'] = 'failed' if failed else 'passed'; save()
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
