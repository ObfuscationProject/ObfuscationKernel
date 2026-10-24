#!/usr/bin/env python3
"""Run bounded UDP association models and configured x86_64 hosted dispatch."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import signal
import stat
import subprocess

DIRECTORIES = ('core', 'apps', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
               'net', 'posix', 'sched', 'smp', 'syscall', 'user')
TESTS = (
    ('network_wire', 'NETWORK_WIRE_PASS'),
    ('native_udp', 'NATIVE_UDP_MODEL_PASS'),
    ('native_udp_binding', 'NATIVE_UDP_BINDING_MODEL_PASS'),
    ('native_udp_connected', 'NATIVE_UDP_CONNECTED_MODEL_PASS'),
    ('linux_udp_connected_capture', 'LINUX_UDP_CONNECTED_CAPTURE_PASS'),
    ('linux_udp_connected_runtime', 'LINUX_UDP_CONNECTED_RUNTIME_PASS'),
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def forest(root):
    members = {}
    for path in sorted(root.rglob('*')):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise ValueError('source symlink: ' + str(path))
        if stat.S_ISREG(mode):
            members[path.relative_to(root).as_posix()] = {
                'sha256': sha(path), 'mode': oct(stat.S_IMODE(mode))}
        elif not stat.S_ISDIR(mode):
            raise ValueError('source special member: ' + str(path))
    return members


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--test', choices=('all',) + tuple(row[0] for row in TESTS), default='all')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        raise ValueError('jobs must be positive')
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    project = out / 'project'
    project.mkdir()
    record = {'schema': 1, 'status': 'preparing', 'sanitizer': args.sanitize,
              'scope': 'Bounded association mechanism hosted tests only; no guest, NIC, SDK, hardware, desktop or full M0-M12 acceptance.',
              'commands': [], 'rows': []}

    def save():
        temporary = out / 'result.json.tmp'
        temporary.write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')
        temporary.replace(out / 'result.json')

    def run(argv, label):
        environment = dict(os.environ, XMAKE_ROOT='y',
                           ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                     'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            environment.pop(name, None)
        command = list(map(str, argv))
        process = subprocess.Popen(command, cwd=project, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            output, _ = process.communicate()
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        try:
            os.killpg(process.pid, 0)
            retired = False
        except ProcessLookupError:
            retired = True
        log = out / (label + '.log')
        log.write_bytes(output)
        record['commands'].append({'argv': command, 'cwd': str(project), 'returncode': process.returncode,
                                   'owned_session_pid': process.pid, 'owned_process_group_retired': retired,
                                   'log': log.name, 'log_sha256': sha(log)})
        save()
        if process.returncode or not retired:
            raise RuntimeError(label + ' failed; retained ' + str(log))
        return output.decode()

    save()
    try:
        source.mkdir()
        trees = [root]
        os_root = root.parent / 'ObfuscationOS'
        if os_root.is_dir():
            trees.append(os_root)
        for tree in trees:
            before = forest(tree)
            shutil.copytree(tree, source / tree.name, copy_function=shutil.copy2)
            if forest(tree) != before or forest(source / tree.name) != before:
                raise ValueError('source changed during complete copy: ' + str(tree))
        manifest = forest(source)
        record['source_members'] = len(manifest)
        record['complete_source_manifest'] = manifest
        kernel = source / root.name
        common = []
        for directory in DIRECTORIES:
            common.extend(path for path in sorted((kernel / 'src' / directory).rglob('*.cpp'))
                          if path.name not in ('entry.cpp', 'kernel_main.cpp', 'runtime.cpp'))
        common += [kernel / 'src/arch/arch.cpp', kernel / 'src/arch/x86_64/ops.cpp']
        cxx = Path(shutil.which(args.cxx) or args.cxx).resolve()
        xmake = Path(shutil.which('xmake') or 'missing-xmake').resolve()
        if not cxx.is_file() or not xmake.is_file():
            raise ValueError('xmake and compiler are required')
        record['tools'] = {str(path): sha(path) for path in (cxx, xmake)}
        flags = ['-frtti', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                 '-ffunction-sections', '-fdata-sections']
        link_flags = ['-Wl,--gc-sections']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
            link_flags += ['-fsanitize=address,undefined', '-no-pie']
        quote = lambda values: ', '.join(json.dumps(str(value)) for value in values)
        lua = ['set_project("hosted-linux-udp-connected")', 'set_languages("c++23")',
               'set_optimize("fast")', 'toolchain("signal-host")', 'set_kind("standalone")',
               'set_toolset("cxx", ' + json.dumps(str(cxx)) + ')',
               'set_toolset("ld", ' + json.dumps(str(cxx)) + ')', 'toolchain_end()',
               'set_toolchains("signal-host")', 'add_defines("OK_ARCH_TARGET_X86_64")',
               'add_includedirs(' + json.dumps(str(kernel / 'include')) + ')',
               'add_cxxflags(' + quote(flags) + ', {force=true})',
               'add_ldflags(' + quote(link_flags) + ', {force=true})',
               'target("runtime-common")', 'set_kind("static")',
               'add_files(' + quote(common) + ')', 'target_end()']
        selected = TESTS if args.test == 'all' else tuple(row for row in TESTS if row[0] == args.test)
        for name, _ in selected:
            lua += ['target(' + json.dumps(name) + ')', 'set_kind("binary")',
                    'set_targetdir("bin")', 'add_deps("runtime-common")',
                    'add_files(' + json.dumps(str(kernel / 'tests/host' / (name + '_tests.cpp'))) + ')',
                    'target_end()']
        (project / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        record['generated_project_sha256'] = sha(project / 'xmake.lua')
        record['scope'] = 'Serialized service association and genuine shared capture/canonical OFD models for ten direct profiles and three socketcall profiles; actual configured x86_64 hosted runtime dispatch. Synthetic traps, memory, page mappings and link device; no target/native execution, guest, NIC, SDK, hardware, desktop or full M0-M12 acceptance. Connected default zero-destination output is supported; explicit zero-destination send and ingress remain rejected.'
        record['status'] = 'running'
        save()
        run([xmake, 'f', '-P', project, '-c', '-y', '--ccache=n'], 'configure')
        run([xmake, 'build', '-P', project, '-j' + str(args.jobs), '-v', '--all'], 'build')
        for name, marker in selected:
            binary = project / 'bin' / name
            output = run([binary], name + '-run')
            if marker not in output:
                raise ValueError('missing exact success marker: ' + name)
            record['rows'].append({'test': name, 'passed': True, 'binary_sha256': sha(binary),
                                   'output': output.strip()})
            save()
            print('PASS', name, output.strip(), flush=True)
        if forest(source) != manifest:
            raise ValueError('complete retained source changed during test')
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()


if __name__ == '__main__':
    main()
