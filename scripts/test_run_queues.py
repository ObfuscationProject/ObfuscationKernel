#!/usr/bin/env python3
"""Hosted model test of the per-CPU run queues (feat/smp-bringup) under all ten profiles.

Model coverage only: host threads stand in for CPUs; no AP starts, no IPI is
sent and no guest or hardware SMP executes.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ARCHITECTURES = ('i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32', 'loongarch64', 'mips', 'mips64', 'ppc')
SOURCES = ('tests/host/run_queue_tests.cpp', 'src/sched/scheduler.cpp', 'src/sched/process.cpp',
           'src/memory/memory.cpp', 'src/net/native_udp.cpp', 'src/fs/vfs.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='new directory for retained sources and logs (default: temporary)')
    parser.add_argument('--arch', choices=('all', *ARCHITECTURES), default='all')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    if args.output is None:
        with tempfile.TemporaryDirectory(prefix='ok-run-queues-') as directory:
            args.output = Path(directory) / 'run-queues'
            return run_suite(args)
    return run_suite(args)


def run_suite(args):
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    architectures = ARCHITECTURES if args.arch == 'all' else (args.arch,)
    record = dict(status='preparing', scope='model: hosted per-CPU run queues and scheduler CPU records; host threads model CPUs; no AP start, IPI, guest or hardware SMP evidence',
                  sanitizer=args.sanitize, leak_detection=False, architectures=list(architectures), source_sha256={}, commands=[], rows=[])

    def save():
        temporary = out / 'result.json.tmp'
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        temporary.replace(out / 'result.json')

    def run(argv, label):
        env = dict(os.environ, XMAKE_ROOT='y', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(name, None)
        completed = subprocess.run(list(map(str, argv)), cwd=source, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=600)
        log = out / (label + '.log')
        log.write_bytes(completed.stdout)
        record['commands'].append(dict(argv=list(map(str, argv)), returncode=completed.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if completed.returncode:
            raise RuntimeError(label + ' failed; see retained log')
        return completed.stdout.decode()

    save()
    try:
        files = {root / name for name in SOURCES}
        files.update(root / f'src/arch/{arch}/ops.cpp' for arch in ARCHITECTURES)
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
        if not (source / 'include/ok/sched/run_queue.hpp').is_file():
            raise ValueError('run queue primitive must be present before snapshot')
        cxx = Path(shutil.which(args.cxx) or args.cxx).resolve()
        xmake = Path(shutil.which('xmake') or '').resolve()
        if not cxx.is_file() or not xmake.is_file():
            raise ValueError('explicit host compiler and xmake are required')
        record['tool_sha256'] = {str(path): sha(path) for path in (cxx, xmake)}
        flags = ['-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-ffunction-sections', '-fdata-sections']
        ldflags = ['-pthread', '-Wl,--gc-sections']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie']
            ldflags += ['-fsanitize=address,undefined', '-no-pie']
        lines = ['set_project("hosted-run-queues")', 'set_languages("c++23")', 'set_optimize("fast")',
                 'toolchain("run-queue-host")', '    set_kind("standalone")',
                 '    set_toolset("cxx", ' + json.dumps(str(cxx)) + ')',
                 '    set_toolset("ld", ' + json.dumps(str(cxx)) + ')', 'toolchain_end()']
        for arch in architectures:
            sources = [*SOURCES, f'src/arch/{arch}/ops.cpp']
            lines += [f'target("run-queues-{arch}")', '    set_kind("binary")', '    set_toolchains("run-queue-host")',
                      '    add_includedirs("include")', '    set_targetdir("bin")', f'    add_defines("OK_ARCH_TARGET_{arch.upper()}")',
                      '    add_cxxflags(' + ', '.join(json.dumps(flag) for flag in flags) + ', {force = true})',
                      '    add_ldflags(' + ', '.join(json.dumps(flag) for flag in ldflags) + ', {force = true})',
                      '    add_files(' + ', '.join(json.dumps(name) for name in sources) + ')', 'target_end()']
        project = source / 'xmake.lua'
        project.write_text('\n'.join(lines) + '\n')
        record['generated_project_sha256'] = sha(project)
        record['status'] = 'running'
        save()
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        for arch in architectures:
            run([xmake, 'build', '-P', source, '-j2', '-v', 'run-queues-' + arch], arch + '-build')
            binary = source / 'bin' / ('run-queues-' + arch)
            output = run([binary], arch + '-run')
            marker = output.rstrip().splitlines()[-1] if output.strip() else ''
            if not marker.startswith('OK_RUN_QUEUES scope=model ') or not marker.endswith('guest=0 hardware_smp=0'):
                raise ValueError('missing run queue success marker')
            record['rows'].append(dict(architecture=arch, status='passed', binary_sha256=sha(binary), output=output.strip()))
            save()
            print('PASS', arch, marker, flush=True)
        for relative, digest in record['source_sha256'].items():
            if sha(source / relative) != digest:
                raise ValueError('retained source changed: ' + relative)
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()


if __name__ == '__main__':
    main()
