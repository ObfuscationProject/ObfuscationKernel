#!/usr/bin/env python3
"""Fresh xmake qualification of bounded same-block rename relocation models."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import traceback
from test_ext4_owned import PROFILES, TARGET_FLAGS

SOURCE = Path(__file__).resolve().parents[1]
LANE = SOURCE.parents[1]
WORKSPACE = LANE.parents[2]
XMAKE = Path('/home/tab/.local/bin/xmake')
HOST = Path('/usr/bin/x86_64-linux-gnu-g++-13')
LIMIT = Path('/usr/bin/prlimit')
SOURCES = ('src/fs/ext4_recovery.cpp', 'src/memory/memory.cpp', 'src/net/native_udp.cpp',
           'src/sched/scheduler.cpp', 'src/sched/process.cpp', 'src/posix/posix.cpp',
           'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp',
           'src/fs/vfs.cpp', 'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp',
           'src/arch/x86_64/ops.cpp', 'src/syscall/linux.cpp', 'tests/host/ext4_realtime_model.cpp')
SUITES = {
    'planner': ('ext4_rename_relocation_tests.cpp', False, 'PASS hosted bounded directory relocation:'),
    'owner': ('ext4_owned_rename_relocation_tests.cpp', False, 'PASS relocation owner:'),
    'before': ('ext4_owned_rename_relocation_fault_tests.cpp', False, 'PASS relocation before effect:'),
    'after': ('ext4_owned_rename_relocation_after_effect_tests.cpp', False, 'PASS relocation after effect:'),
    'replay': ('ext4_directory_relocation_replay_tests.cpp', False, 'PASS private relocation replay:'),
    'fit-replay': ('ext4_directory_replay_tests.cpp', False, 'PASS private directory replay:'),
    'fit-owner': ('ext4_owned_rename_tests.cpp', False, 'PASS private mounted rename owner:'),
    'fit-before': ('ext4_owned_rename_fault_tests.cpp', False, 'PASS private mounted rename owner:'),
    'fit-after': ('ext4_owned_rename_after_effect_tests.cpp', False, 'PASS rename after effect:'),
    'legacy-planner': ('ext4_rename_plan_tests.cpp', False, 'PASS bounded rename plan:'),
    'debug': ('ext4_rename_relocation_debug_tests.cpp', False, 'PASS hosted directory debug point:')}

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def inventory(root):
    rows = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink(): raise ValueError('symlink in frozen forest: ' + str(path))
        if path.is_file(): rows[path.relative_to(root).as_posix()] = sha(path)
    return rows

def need(value, message):
    if not value: raise ValueError(message)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', required=True)
    args = parser.parse_args()
    need(sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1', 'disable bytecode writes')
    need(args.attempt.isdigit() and len(args.attempt) == 2, 'explicit two-digit attempt required')
    out = LANE / ('qualification-attempt-' + args.attempt)
    out.mkdir(exist_ok=False)
    report = dict(schema=1, status='running', scope='hosted memory-disk relocation, replay and fault models',
        commands=[], markers={}, profiles=[], native_execution=False, physical=False, sdk_export=False,
        os_consumer=False, installed=False, promoted=False, full_m0_m12=False, leak_check=False)
    def save(): (out / 'RESULT.json').write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')
    save()
    try:
        parent_manifest = LANE.parent / 'sdk51-v38-active-recovery-v1/SOURCE_MANIFEST_V1.json'
        need(sha(parent_manifest) == 'ee16547ac6be5f2f4d94bc0e103cf77df28398240ced7f77911cd1a221d74843', 'exact SDK51 parent manifest changed')
        parent = json.loads(parent_manifest.read_text())
        parent_source = Path(parent['kernel_source_path'])
        need(inventory(parent_source) == parent['kernel_source_files_sha256'], 'SDK51 parent source drift')
        mapping = inventory(SOURCE)
        report.update(source_count=len(mapping), source_files_sha256=mapping,
            parent_manifest_sha256=sha(parent_manifest), parent_source_count=parent['kernel_source_count'],
            source_delta=[name for name in mapping if mapping[name] != parent['kernel_source_files_sha256'].get(name)],
            source_removed=sorted(set(parent['kernel_source_files_sha256']) - set(mapping)))
        need(not report['source_removed'], 'useful parent source removed')
        snapshot = out / 'source/ObfuscationKernel'
        shutil.copytree(SOURCE, snapshot)
        need(inventory(snapshot) == mapping, 'source copy differs')
        inputs = out / 'inputs'; inputs.mkdir()
        cleanup = LANE.parent / 'sdk51-v38-active-campaign-candidate-v1/final-source-v2/cleanup_owner_v2.py'
        need(sha(cleanup) == 'eca5487eac9d409472a712c59b737b2afd70160d39a1879f209570069495907e', 'accepted cleanup helper changed')
        shutil.copyfile(cleanup, inputs / cleanup.name)
        for path in (parent_manifest, LANE / 'DESIGN_V1.md', LANE / 'ROOT_DESIGN_REVIEW_V1.json'):
            shutil.copyfile(path, inputs / path.name)
        spec = importlib.util.spec_from_file_location('cleanup', inputs / cleanup.name)
        owner = importlib.util.module_from_spec(spec); spec.loader.exec_module(owner)
        owner.set_subreaper()
        fixtures = out / 'fixtures'; fixtures.mkdir()
        pins = {
            'clean-input.img': (LANE.parent / 'sdk49-v33-history-route-guard-v1/qualification-attempt-02/fixtures/clean-input.img',
                                '9611c691bc656bee8f70d256f35e9a90e640bef663143a4215e0403c9b8a678a'),
            'metadata-published.img': (LANE.parent / 'sdk49-v33-history-route-guard-v1/qualification-attempt-02/model-plain/published.img',
                                       'ee701c172cf032e32b4062ba46a7a583001002b58d11a06731f9e26c0a9aa58a')}
        for name, (path, digest) in pins.items():
            need(sha(path) == digest, 'fixture changed: ' + str(path)); shutil.copyfile(path, fixtures / name)
        report['fixtures_sha256'] = inventory(fixtures)
        env = dict(os.environ, XMAKE_GLOBALDIR=str(out / 'xmake-global'), TMPDIR=str(out / 'tmp'),
                   XMAKE_COLORTERM='nocolor', XMAKE_ROOT='y', PYTHONDONTWRITEBYTECODE='1', TZ='UTC', LC_ALL='C')
        for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                    'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'PYTHONPATH', 'PYTHONHOME',
                    'XMAKE_RCFILES', 'XMAKE_CONFIGDIR', 'XMAKE_PROGRAMDIR', 'XMAKE_PLAT',
                    'XMAKE_ARCH', 'MKE2FS_CONFIG', 'LD_PRELOAD', 'LD_LIBRARY_PATH'):
            env.pop(key, None)
        for name in ('xmake-global', 'tmp', 'objects'): (out / name).mkdir()
        project = out / 'project'; project.mkdir(); build = out / 'build'
        report['tools_sha256'] = {str(path): sha(path.resolve()) for path in (XMAKE, HOST, LIMIT)}
        def run(label, argv, child_env=env, timeout=900):
            command = [str(LIMIT), '--stack=8388608:8388608', '--core=0:0', '--', *map(str, argv)]
            row = dict(label=label, argv=command, timeout_seconds=timeout)
            report['commands'].append(row); save()
            log = out / (label + '.log'); process = None; expired = False; started = time.monotonic()
            with log.open('xb') as stream:
                try:
                    process = subprocess.Popen(command, cwd=project, env=child_env, stdin=subprocess.DEVNULL,
                        stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                    process.wait(timeout=timeout)
                except subprocess.TimeoutExpired: expired = True
                finally:
                    if process is not None: row['cleanup'] = owner.stop_owned_tree(process)
                    else: need(not owner.owned_descendants(), 'unexpected child forest')
            row.update(exit_code=process.returncode, timed_out=expired,
                       seconds=time.monotonic() - started, log_sha256=sha(log)); save()
            need(not expired and process.returncode == 0, label + ' failed; retained log')
            print('PASS ' + label, flush=True)
            return log.read_text()
        lines = ['set_project("DirectoryRenameRelocation")', 'set_languages("c++23")', 'add_rules("mode.debug")']
        def target(name, kind, files, sanitized=False, private=False, dependency=None):
            lines.extend(['target(' + json.dumps(name) + ')', 'set_kind(' + json.dumps(kind) + ')',
                'add_files(' + ', '.join(json.dumps(str(path)) for path in files) + ')',
                'add_includedirs(' + json.dumps(str(snapshot / 'include')) + ')',
                'add_defines("OK_ARCH_TARGET_X86_64")',
                'add_cxxflags("-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", '
                '"-ffunction-sections", "-fdata-sections", "-fno-exceptions", "-frtti", {force=true})'])
            if private: lines.append('add_cxxflags("-fno-access-control", {force=true})')
            if sanitized:
                lines.extend(['add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force=true})',
                              'add_ldflags("-fsanitize=address,undefined", {force=true})'])
            if kind == 'binary':
                lines.extend(['add_ldflags("-no-pie", "-Wl,--gc-sections", "-Wl,-l:libext2fs.so.2", {force=true})',
                              'add_syslinks("pthread")'])
            if dependency: lines.append('add_deps(' + json.dumps(dependency) + ')')
            lines.append('target_end()')
        for mode in ('plain', 'sanitized'):
            san = mode == 'sanitized'
            target('core-' + mode, 'static', [snapshot / name for name in SOURCES], san)
            for suite, (filename, private, _) in SUITES.items():
                target(suite + '-' + mode, 'binary', [snapshot / 'tests/host' / filename], san, private,
                       None if suite in ('planner', 'debug') else 'core-' + mode)
        (project / 'xmake.lua').write_text('\n'.join(lines) + '\n')
        run('configure', [XMAKE, 'f', '-P', project, '-y', '-m', 'debug', '--builddir=' + str(build),
                          '--ccache=n', '--cxx=' + str(HOST), '--ld=' + str(HOST)])
        run('build', [XMAKE, 'build', '-P', project, '-v', '-j', '2'], timeout=1800)
        binaries = build / 'linux/x86_64/debug'
        for mode in ('plain', 'sanitized'):
            child = dict(env)
            if mode == 'sanitized': child.update(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
            for suite, (_, _, prefix) in SUITES.items():
                if suite in ('planner', 'debug'): arguments = []
                elif suite == 'legacy-planner': arguments = [fixtures / 'clean-input.img', fixtures / 'metadata-published.img', out / ('legacy-' + mode + '.img')]
                elif suite in ('before', 'fit-before'): arguments = [fixtures / 'clean-input.img']
                elif suite == 'fit-owner': arguments = [fixtures / 'clean-input.img', out / ('fit-owner-' + mode + '-published.img'),
                                                       out / ('fit-owner-' + mode + '-completed.img')]
                else: arguments = [fixtures / 'clean-input.img', out / (suite + '-' + mode + '-media' + ('.img' if 'replay' in suite else ''))]
                content = run(suite + '-' + mode + '-run', [binaries / (suite + '-' + mode), *arguments], child, timeout=1800)
                markers = [line for line in content.splitlines() if line.startswith(prefix)]
                need(markers, 'required marker missing: ' + suite + ': ' + content[-600:])
                report['markers'][suite + '-' + mode] = markers; save()
        for suite in SUITES:
            need(report['markers'][suite + '-plain'] == report['markers'][suite + '-sanitized'], 'model marker mismatch: ' + suite)
        for arch, triple, elf_class, endian, machine in PROFILES:
            compiler = WORKSPACE / 'ObfuscationKernel/toolchains' / triple / 'bin' / (triple + '-g++')
            obj = out / 'objects' / (arch + '.o')
            run('cross-' + arch, [compiler, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                '-ffreestanding', '-fno-exceptions', '-frtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit',
                *TARGET_FLAGS[arch], '-DOK_ARCH_TARGET_' + arch.upper(), '-I' + str(snapshot / 'include'), '-c',
                snapshot / 'tests/host/ext4_rename_relocation_compile.cpp', '-o', obj])
            data = obj.read_bytes(); order = '<' if endian == 1 else '>'
            need(data[:4] == b'\x7fELF' and data[4:6] == bytes((elf_class, endian)) and
                 struct.unpack_from(order + 'H', data, 18)[0] == machine, 'cross ELF identity: ' + arch)
            report['profiles'].append(dict(architecture=arch, object_sha256=sha(obj), compiler_sha256=sha(compiler.resolve()), executed=False)); save()
        need(inventory(SOURCE) == mapping and inventory(snapshot) == mapping, 'source changed during qualification')
        need(inventory(fixtures) == report['fixtures_sha256'], 'immutable fixture changed')
        report['status'] = 'passed-hosted-relocation-models-and-ten-profile-freestanding-compile'
    except BaseException as error:
        report.update(status='failed-preserved-hosted-relocation-qualification', error=repr(error), traceback=traceback.format_exc())
    save()
    if any(row.get('cleanup', {}).get('verified') is not True for row in report['commands']):
        report['status'] = 'failed-preserved-unfrozen-cleanup-unverified'; save(); return 1
    frozen = inventory(out)
    (out / 'FREEZE.json').write_text(json.dumps(dict(schema=1, status='frozen-complete-private-hosted-forest',
        result_sha256=sha(out / 'RESULT.json'), files_sha256=frozen), sort_keys=True, indent=2) + '\n')
    print(report['status'], flush=True)
    return 0 if report['status'].startswith('passed-') else 1

if __name__ == '__main__': raise SystemExit(main())
