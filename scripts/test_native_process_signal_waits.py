#!/usr/bin/env python3
"""Replay inherited actual wait tests with shared process enqueue; hosted only."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import resource
import shutil
import subprocess

CASES = ('pipe_signal', 'pipe_write_signal', 'pipe_vector_signal', 'child_wait_signal',
         'futex_signal', 'poll_signal', 'poll_temporary_mask', 'sleep_interruption', 'pipe_blocking_write')
DIRECTORIES = ('core', 'rescue', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory', 'net', 'posix',
               'sched', 'smp', 'syscall', 'user')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def shared_calls(source):
    needle = 'native::private_signal_queue('
    while needle in source:
        start = source.index(needle)
        cursor, depth = start + len(needle), 1
        argument = cursor
        while depth:
            if source[cursor] == '(':
                depth += 1
            elif source[cursor] == ')':
                depth -= 1
            cursor += 1
        call = 'native::private_signal_shared(kernel, (' + source[argument:cursor-1] + ').owner)'
        source = source[:start] + call + source[cursor:]
    return source


def transform(case, original):
    source = original
    if case == 'poll_signal':
        begin = source.index('void enqueue(')
        end = source.index('void publication(', begin)
        source = source[:begin] + '''void enqueue(Kernel &kernel, Group group, u32 signal = 10)
{
    auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    expect(native::private_process_kill(kernel, *process, group.waiter, signal) == 0);
}
''' + source[end:]
    if case == 'child_wait_signal':
        # The inherited sender case uses raw tkill; the shared variant uses kill.
        source = source.replace('from->native_frame.registers[14]=200;', 'from->native_frame.registers[14]=62;')
    if case == 'pipe_blocking_write':
        begin = source.index('void capture_identity_signal(')
        source = source[:begin] + shared_calls(source[begin:])
    else:
        source = shared_calls(source)
    source = source.replace('private_signal_shared(kernel, (*current).owner)',
                            'private_signal_shared(*native::runtime_kernel, (*current).owner)')
    source = source.replace('native::PrivateSignalQueue', 'native::PrivateProcessQueue')
    source = source.replace(', -6,', ', 0,').replace(',-6,', ',0,')
    # Labels preserve inherited assertions; provenance identifies shared routing.
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    record = dict(status='running', scope=__doc__, source_sha256={}, generated_tests={}, commands=[], rows=[],
                  sanitizer=True, native_executions=0, independent_cases=False,
                  attribution='Inherited wait assertions adapted to shared enqueue; actual hosted runtime.')
    def save():
        (out / 'result.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    def run(argv, label):
        command = list(map(str, argv))
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out / 'xmake-global'),
                   ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(name, None)
        log = out / (label + '.log')
        with log.open('wb') as stream:
            done = subprocess.run(command, cwd=source, env=env, stdout=stream, stderr=subprocess.STDOUT)
        record['commands'].append(dict(argv=command, returncode=done.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if done.returncode:
            raise RuntimeError(str(log))
        return log.read_text()
    save()
    try:
        support = []
        for directory in DIRECTORIES:
            support.extend(str(p.relative_to(root)) for p in sorted((root / 'src' / directory).rglob('*.cpp'))
                           if p.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp'))
        support += ['src/arch/arch.cpp', 'src/arch/x86_64/ops.cpp']
        files = {root / n for n in support} | {root / 'src/user/runtime.cpp', Path(__file__).resolve()}
        files.update(root / ('tests/host/native_' + case + '_tests.cpp') for case in CASES)
        for directory in ('include', 'src', 'tests'):
            files.update(p for p in (root / directory).rglob('*') if p.is_file() and p.suffix in ('.hpp', '.h', '.inc'))
        for path in sorted(files):
            name = str(path.relative_to(root)); target = source / name; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target); record['source_sha256'][name] = sha(target)
            if sha(path) != sha(target):
                raise ValueError('source changed: ' + name)
        for case in CASES:
            name = 'tests/host/native_' + case + '_tests.cpp'
            generated = 'tests/host/process_shared_' + case + '.cpp'
            (source / generated).write_text(transform(case, (source / name).read_text()))
            record['generated_tests'][case] = dict(original=name, generated=generated, sha256=sha(source / generated))
        cxx = Path(shutil.which('g++')).resolve(); xmake = Path(shutil.which('xmake')).resolve()
        record['tools'] = {str(p): sha(p) for p in (cxx, xmake)}
        flags = ['-O1', '-frtti', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-ffunction-sections', '-fdata-sections',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
        lua = ['set_project("shared-process-signal-waits")', 'set_languages("c++23")', 'toolchain("host")',
               'set_kind("standalone")', 'set_toolset("cxx", ' + json.dumps(str(cxx)) + ')',
               'set_toolset("ld", ' + json.dumps(str(cxx)) + ')', 'toolchain_end()', 'set_toolchains("host")',
               'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
               'add_cxxflags(' + ','.join(map(json.dumps, flags)) + ', {force=true})',
               'target("support")', 'set_kind("static")',
               'add_files(' + ','.join(map(json.dumps, support)) + ')', 'target_end()']
        for case in CASES:
            lua += ['target(' + json.dumps(case) + ')', 'set_kind("binary")', 'set_targetdir("bin")', 'add_deps("support")',
                    'add_files(' + json.dumps(record['generated_tests'][case]['generated']) + ')',
                    'add_ldflags("-fsanitize=address,undefined", "-no-pie", "-Wl,--gc-sections", {force=true})']
            if case == 'pipe_blocking_write':
                lua += ['add_defines("OK_BLOCKING_WRITE_PRIVATE_TEST")']
            lua += ['target_end()']
        (source / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        record['generated_project_sha256'] = sha(source / 'xmake.lua')
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        for case in CASES:
            run([xmake, 'build', '-P', source, '-j2', '-v', case], case + '-build')
            binary = source / 'bin' / case
            output = run([binary], case + '-run')
            if 'PASS' not in output or 'checks=' not in output:
                raise ValueError('missing inherited marker: ' + case)
            record['rows'].append(dict(case=case, status='passed', binary_sha256=sha(binary), output=output.strip()))
            save(); print('SHARED_WAIT_PASS', case, flush=True)
        for name, digest in record['source_sha256'].items():
            if sha(source / name) != digest:
                raise ValueError('retained source changed: ' + name)
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error)); save(); raise
    save()


if __name__ == '__main__':
    main()
