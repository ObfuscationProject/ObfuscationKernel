#!/usr/bin/env python3
"""Run preserved pure restart and typed pipe/futex/child publication regressions."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess

DIRECTORIES = ('core', 'apps', 'driver', 'fs', 'gui', 'interrupt', 'ipc', 'memory',
               'net', 'posix', 'sched', 'smp', 'syscall', 'user')
TESTS = (
    ('native_syscall_restart', 'RESTART_ADAPTER_PASS'),
    ('native_syscall_restart_publication', 'RESTART_PUBLICATION_PASS'),
    ('native_futex_restart_publication', 'FUTEX_PUBLICATION_PASS'),
    ('native_child_wait_restart_publication', 'CHILD_PUBLICATION_PASS'),
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
    parser.add_argument('--test', choices=('all', 'native_syscall_restart', 'native_syscall_restart_publication', 'native_futex_restart_publication', 'native_child_wait_restart_publication'), default='all')
    parser.add_argument('--ppc-model-elf-source', type=Path,
                        help='Explicit static SYSV PPC32 ELF; retained Linux OSABI copy is model metadata only')
    parser.add_argument('--ppc-model-elf-source-sha256')
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
              'scope': 'Hosted actual x86_64 runtime, captured ownership, scheduler and POSIX OFD; '
                       'synthetic traps, mappings and link frames. Target result models preserve ten profiles. '
                       'Captured ten-profile receive/restart models and n64/PPC typed publication are hosted only. '
                       'Restart runtime/rt_sigreturn execution is x86_64 hosted only. No guest, NIC, SDK construction, '
                       'other native target restart, hardware or full M0-M12 acceptance.',
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
        result = subprocess.run(command, cwd=project, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = out / (label + '.log')
        log.write_bytes(result.stdout)
        record['commands'].append({'argv': command, 'cwd': str(project), 'returncode': result.returncode,
                                   'log': log.name, 'log_sha256': sha(log)})
        save()
        if result.returncode:
            raise RuntimeError(label + ' failed; retained ' + str(log))
        return result.stdout.decode()

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
        lua = ['set_project("hosted-linux-udp-signal")', 'set_languages("c++23")',
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
        model_elf_arguments = []
        if any(name.endswith('_publication') for name, _ in selected):
            if args.ppc_model_elf_source is None or not args.ppc_model_elf_source_sha256:
                raise ValueError('publication needs an explicit hash-pinned PPC model ELF source')
            original = args.ppc_model_elf_source.absolute()
            if original.is_symlink() or not original.is_file() or sha(original) != args.ppc_model_elf_source_sha256:
                raise ValueError('PPC model ELF source is not the exact pinned regular file')
            metadata = out / 'model-metadata'
            metadata.mkdir()
            retained = metadata / 'source-sysv.elf'
            shutil.copy2(original, retained)
            if sha(original) != args.ppc_model_elf_source_sha256 or sha(retained) != args.ppc_model_elf_source_sha256:
                raise ValueError('PPC model ELF source changed while retained')
            raw = bytearray(retained.read_bytes())
            if (len(raw) < 52 or raw[:7] != b'\x7fELF\x01\x02\x01' or raw[7] != 0 or
                    int.from_bytes(raw[16:18], 'big') != 2 or int.from_bytes(raw[18:20], 'big') != 20):
                raise ValueError('PPC model source needs static SYSV ELF32 big-endian PPC executable identity')
            raw[7] = 3
            derived = metadata / 'linux-osabi-model.elf'
            derived.write_bytes(raw)
            record['ppc_model_metadata'] = {
                'source': str(original), 'source_sha256': args.ppc_model_elf_source_sha256,
                'source_mode': oct(stat.S_IMODE(original.stat().st_mode)),
                'retained_source': str(retained), 'retained_source_sha256': sha(retained),
                'retained_source_mode': oct(stat.S_IMODE(retained.stat().st_mode)),
                'derived': str(derived), 'derived_sha256': sha(derived),
                'derived_mode': oct(stat.S_IMODE(derived.stat().st_mode)),
                'transformation': 'Only ELF EI_OSABI byte 7 changes from SYSV 0 to Linux 3 in a fresh copy',
                'metadata_eligibility_only': True, 'code_loading_or_execution': False,
                'original_modified': False}
            model_elf_arguments = [derived]
        for name, _ in selected:
            lua += ['target(' + json.dumps(name) + ')', 'set_kind("binary")',
                    'set_targetdir("bin")', 'add_deps("runtime-common")',
                    'add_files(' + json.dumps(str(kernel / 'tests/host' / (name + '_tests.cpp'))) + ')',
                    'target_end()']
        (project / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        record['generated_project_sha256'] = sha(project / 'xmake.lua')
        record['scope'] = 'Preserved hosted ten-profile pure restart adapters and n64/PPC pipe/futex/child restart-publication models. Synthetic backend qualification and mappings; no target execution, guest, NIC, SDK, hardware, desktop or full M0-M12 acceptance.'
        record['status'] = 'running'
        save()
        run([xmake, 'f', '-P', project, '-c', '-y', '--ccache=n'], 'configure')
        run([xmake, 'build', '-P', project, '-j' + str(args.jobs), '-v', '--all'], 'build')
        for name, marker in selected:
            binary = project / 'bin' / name
            output = run([binary, *(model_elf_arguments if name.endswith('_publication') else [])], name + '-run')
            if marker not in output:
                raise ValueError('missing exact success marker: ' + name)
            record['rows'].append({'test': name, 'passed': True, 'binary_sha256': sha(binary),
                                   'output': output.strip()})
            save()
            print('PASS', name, output.strip(), flush=True)
            if name == 'linux_udp_restart_runtime':
                label = name + '-retirement-failure'
                output = run([binary, '--retirement-failure'], label + '-run')
                if 'LINUX_UDP_RESTART_RETIREMENT_FAILURE_PASS' not in output:
                    raise ValueError('missing restart terminal retirement failure marker')
                record['rows'].append({'test': label, 'passed': True, 'binary_sha256': sha(binary),
                                       'output': output.strip()})
                save()
                print('PASS', label, output.strip(), flush=True)
            if name == 'linux_udp_signal_runtime':
                for failure in ('publication', 'retirement', 'combined'):
                    label = name + '-' + failure
                    output = run([binary, '--failure', failure], label + '-run')
                    marker = 'LINUX_UDP_SIGNAL_POST_CONSUME_FAILURE_PASS mode=' + failure
                    if marker not in output:
                        raise ValueError('missing exact terminal failure marker: ' + failure)
                    record['rows'].append({'test': label, 'passed': True, 'binary_sha256': sha(binary),
                                           'output': output.strip()})
                    save()
                    print('PASS', label, output.strip(), flush=True)
        if forest(source) != manifest:
            raise ValueError('complete retained source changed during test')
        if 'ppc_model_metadata' in record:
            metadata = record['ppc_model_metadata']
            for path_key, hash_key, mode_key in (
                    ('source', 'source_sha256', 'source_mode'),
                    ('retained_source', 'retained_source_sha256', 'retained_source_mode'),
                    ('derived', 'derived_sha256', 'derived_mode')):
                path = Path(metadata[path_key])
                if sha(path) != metadata[hash_key] or oct(stat.S_IMODE(path.stat().st_mode)) != metadata[mode_key]:
                    raise ValueError('PPC model metadata input changed: ' + str(path))
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()


if __name__ == '__main__':
    main()
