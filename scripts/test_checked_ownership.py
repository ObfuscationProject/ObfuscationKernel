#!/usr/bin/env python3
"""Run xmake hosted checked ownership and optional ten-profile object checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

from test_ext4_mounted import SOURCES
from test_ext4_owned import PROFILES, TARGET_FLAGS


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clean-image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--toolchain-root', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source'
    for name in ('include', 'src', 'tests/host', 'tests/native'):
        shutil.copytree(root / name, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    scripts = source / 'scripts'
    scripts.mkdir()
    for name in ('test_checked_ownership.py', 'test_ext4_mounted.py', 'test_ext4_owned.py'):
        shutil.copy2(root / 'scripts' / name, scripts / name)
    inputs = {str(path.relative_to(source)): sha(path) for path in source.rglob('*') if path.is_file()}
    report = dict(status='running', scope='Hosted actual POSIX/VFS/mounted ext4 checked ownership; no guest or hardware',
        sanitized=args.sanitize, commands=[], source_sha256=inputs, binaries={}, profiles={}, tools={})

    def save():
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def pin(path):
        path = Path(path).resolve()
        report['tools'][str(path)] = sha(path)
        return path

    def run(argv, label, cwd=source):
        argv = list(map(str, argv))
        env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global'),
            PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(key, None)
        log = output / (label + '.log')
        with log.open('xb') as stream:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        report['commands'].append(dict(label=label, argv=argv, cwd=str(cwd), returncode=result.returncode,
            log=str(log), log_sha256=sha(log)))
        save()
        if result.returncode:
            raise RuntimeError(str(log))
        return log.read_text()

    try:
        image = output / 'ownership.img'
        shutil.copyfile(args.clean_image, image)
        report['original_image'] = dict(path=str(args.clean_image.resolve()), sha256=sha(args.clean_image))
        payload = output / 'payload'
        payload.write_bytes(b'journalled stored metadata fixture')
        requests = (f'write {payload} /other0', 'symlink /seed-link seed', 'mkdir /child',
            f'write {payload} /child/leaf', 'symlink /child-link child',
            'set_inode_field /seed uid 1000', 'set_inode_field /seed gid 1001', 'set_inode_field /seed mode 0107754',
            'set_inode_field /child uid 1000', 'set_inode_field /child gid 1001', 'set_inode_field /child mode 0044700')
        for index, request in enumerate(requests):
            run(['debugfs', '-w', '-R', request, image], 'fixture-' + str(index))
        run(['e2fsck', '-fn', image], 'fixture-fsck')
        report['input_sha256'] = sha(image)
        compiler, xmake = pin('/usr/bin/g++'), pin(shutil.which('xmake'))
        pin(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip())
        suites = {'ownership': 'ext4_checked_ownership_tests.cpp',
            'invariants': 'ext4_checked_ownership_invariants.cpp', 'recovery': 'ext4_checked_ownership_recovery.cpp',
            'mode-regression': 'ext4_journalled_mode_tests.cpp',
            'rename-regression': 'ext4_owned_rename_tests.cpp',
            'account-writes': 'ext4_account_writes_tests.cpp'}
        project = ['set_project("checked-ownership-hosted")', 'set_languages("cxx23")',
            'target("ownership-common")', 'set_kind("static")', 'add_includedirs("include")',
            'add_defines("OK_ARCH_TARGET_X86_64")',
            'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror",'
            '"-ffunction-sections","-fdata-sections",{force=true})']
        project += ['add_files(' + json.dumps(path) + ')' for path in SOURCES]
        if args.sanitize:
            project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})']
        project += ['target_end()']
        for name, test in suites.items():
            project += ['target(' + json.dumps(name) + ')', 'set_kind("binary")', 'set_targetdir("bin")',
                'add_deps("ownership-common")', 'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
                'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror",'
                '"-ffunction-sections","-fdata-sections",{force=true})',
                'add_ldflags("-Wl,--gc-sections","-Wl,-l:libext2fs.so.2","-pthread",{force=true})']
            project += ['add_files(' + json.dumps('tests/host/' + test) + ')']
            if name == 'invariants':
                project += ['add_cxxflags("-fno-access-control",{force=true})']
            if args.sanitize:
                project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                    'add_ldflags("-fsanitize=address,undefined",{force=true})']
            project += ['target_end()']
        (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
        report['project_sha256'] = sha(source / 'xmake.lua')
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n', '--cxx=' + str(compiler), '--ld=' + str(compiler)], 'configure')
        run([xmake, 'build', '-P', source, '-j2', '-v'], 'build')
        for name in suites:
            binary = source / 'bin' / name
            report['binaries'][name] = dict(path=str(binary), sha256=sha(binary))
            arguments = [binary, image]
            if name == 'rename-regression':
                arguments += [output / 'rename-published.img', output / 'rename-completed.img']
            text = run(arguments, name)
            if name == 'ownership':
                if 'owner_devices=1 owner_buffers=9 guest=0 hardware=0' not in text:
                    raise ValueError('ownership execution marker differs')
            elif name == 'recovery':
                if 'complete_calls=1 torn_media=0 guest=0 hardware=0' not in text:
                    raise ValueError('ownership recovery execution marker differs')
            elif name == 'invariants':
                if 'hosted=1 guest=0 hardware=0' not in text:
                    raise ValueError('ownership invariant execution marker differs')
            elif name == 'mode-regression':
                if 'profiles=10 hosted=1 user_execution=0 hardware=0' not in text:
                    raise ValueError('retained mode execution marker differs')
            elif name == 'account-writes':
                if 'root=1 ordinary=1 production_common_storage=1 hosted=1 guest=0 hardware=0' not in text:
                    raise ValueError('account split-write execution marker differs')
            elif 'PASS private mounted rename owner:' not in text:
                raise ValueError('retained rename execution marker differs')
        if args.toolchain_root:
            for arch, triple, elf_class, endian, machine in PROFILES:
                compiler = pin(args.toolchain_root / triple / 'bin' / (triple + '-g++'))
                work = output / arch
                work.mkdir()
                flags = ['-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-ffreestanding', '-fno-exceptions',
                    '-frtti', '-fno-stack-protector', '-fno-use-cxa-atexit', '-fno-threadsafe-statics', *TARGET_FLAGS[arch]]
                project = ['set_languages("cxx23")', 'toolchain("ownership-target")', 'set_kind("standalone")',
                    'set_toolset("cxx",' + json.dumps(str(compiler)) + ')', 'toolchain_end()',
                    'target("ownership-object")', 'set_kind("object")', 'set_toolchains("ownership-target")',
                    'add_defines("OK_KERNEL_FREESTANDING","OK_ARCH_TARGET_' + arch.upper() + '")',
                    'add_includedirs(' + json.dumps(str(source / 'include')) + ')',
                    'add_files(' + json.dumps(str(source / 'tests/native/checked_ownership_compile.cpp')) + ')',
                    'add_cxxflags(' + ','.join(map(json.dumps, flags)) + ',{force=true})', 'target_end()']
                (work / 'xmake.lua').write_text('\n'.join(project) + '\n')
                run([xmake, 'f', '-P', work, '-c', '-y', '--ccache=n'], arch + '-configure', work)
                run([xmake, 'build', '-P', work, '-j2', '-v'], arch + '-build', work)
                objects = list((work / 'build/.objs/ownership-object').rglob('*.o'))
                if len(objects) != 1:
                    raise ValueError('ownership object count differs: ' + arch)
                data = objects[0].read_bytes()
                if data[:4] != b'\x7fELF' or data[4:6] != bytes((elf_class, endian)) or \
                    struct.unpack_from('<H' if endian == 1 else '>H', data, 18)[0] != machine:
                    raise ValueError('ownership object identity differs: ' + arch)
                report['profiles'][arch] = dict(path=str(objects[0]), sha256=sha(objects[0]))
        if sha(image) != report['input_sha256'] or sha(args.clean_image) != report['original_image']['sha256']:
            raise ValueError('test source image changed')
        for name, digest in inputs.items():
            if sha(source / name) != digest:
                raise ValueError('test source snapshot changed: ' + name)
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print('PASS hosted checked ownership and retained mode regression; guest=0 hardware=0')


if __name__ == '__main__':
    main()
