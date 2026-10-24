#!/usr/bin/env python3
"""Compile the access dispatcher/return boundary for ten explicit Kernel profiles."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess

TRIPLES = {'i386':'i386-elf', 'x86_64':'x86_64-elf', 'aarch64':'aarch64-elf',
           'arm32':'arm-none-eabi', 'rv64':'riscv64-elf', 'rv32':'riscv32-elf',
           'loongarch64':'loongarch64-elf', 'mips':'mips-elf', 'mips64':'mips64-elf', 'ppc':'powerpc-eabi'}
IDENTITY = {'i386':(1,1,3), 'x86_64':(2,1,62), 'aarch64':(2,1,183), 'arm32':(1,1,40),
            'rv32':(1,1,243), 'rv64':(2,1,243), 'loongarch64':(2,1,258),
            'mips':(1,2,8), 'mips64':(2,2,8), 'ppc':(1,2,20)}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--toolchains', required=True, type=Path)
    parser.add_argument('--flag-sdk', required=True, type=Path)
    parser.add_argument('--flag-sdk-abi', required=True, type=int)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input'
    shutil.copytree(root / 'include', source / 'include')
    for name in ('tests/native/linux_access_dispatch_compile.cpp', 'scripts/test_linux_access_targets.py'):
        (source / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, source / name)
    state = dict(status='running', scope='Ten-profile freestanding C++23 dispatcher/completion object compilation only; no kernel link, SDK export or execution.',
                 profile_flag_source='Explicit per-profile debug SDK manifests supply build flags only; prototype source is compiled independently.',
                 flag_sdk_abi=args.flag_sdk_abi, profiles={}, commands=[], qemu=False)
    xmake = Path(shutil.which('xmake')).resolve()
    state['xmake'] = dict(path=str(xmake), sha256=sha(xmake))
    project = ['set_project("linux_access_targets")']
    for arch, triple in TRIPLES.items():
        folder = output / arch
        folder.mkdir()
        manifest = args.flag_sdk.resolve() / arch / 'debug/manifest.json'
        data = json.loads(manifest.read_text())
        assert data['abi_version'] == args.flag_sdk_abi and data['arch'] == arch and data['mode'] == 'debug'
        compiler = (args.toolchains.resolve() / triple / 'bin' / (triple + '-g++')).resolve()
        assert subprocess.check_output([compiler, '-dumpmachine'], text=True).strip() == triple
        cc1 = Path(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip()).resolve()
        target = folder / 'dispatcher.o'
        command = [str(compiler), '-std=c++23', *data['cxxflags'], *['-D' + v for v in data['defines']],
                   '-Werror', '-I' + str(source / 'include'), '-c',
                   str(source / 'tests/native/linux_access_dispatch_compile.cpp'), '-o', str(target)]
        project += ['target(' + json.dumps('access-' + arch) + ')', 'set_kind("phony")',
                    'on_build(function ()', 'os.vrunv(' + json.dumps(command[0]) + ', {' +
                    ', '.join(json.dumps(arg) for arg in command[1:]) + '})', 'end)', 'target_end()']
        state['profiles'][arch] = dict(compiler=str(compiler), compiler_sha256=sha(compiler),
            cc1plus=str(cc1), cc1plus_sha256=sha(cc1), flag_manifest=str(manifest),
            flag_manifest_sha256=sha(manifest), command=command, object=str(target))
    (source / 'xmake.lua').write_text('\n'.join(project) + '\n')
    sources = {str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()}
    state['source_sha256'] = sources

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    save()
    try:
        commands = [('configure', [xmake, 'f', '-P', source, '-c', '-y', '--ccache=n']),
                    ('compile', [xmake, 'build', '-P', source, '-j2', '-v'])]
        for label, command in commands:
            log = output / (label + '.log')
            argv = list(map(str, command))
            with log.open('x') as stream:
                result = subprocess.run(argv, cwd=source, stdout=stream, stderr=subprocess.STDOUT,
                    env=dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(output / 'xmake-global')))
            state['commands'].append(dict(argv=argv, returncode=result.returncode, log=str(log), log_sha256=sha(log)))
            save()
            assert result.returncode == 0, log
        for arch, record in state['profiles'].items():
            target = Path(record['object'])
            data = target.read_bytes()
            assert data[:4] == b'\x7fELF'
            order = '<' if data[5] == 1 else '>'
            kind, machine = struct.unpack_from(order + 'HH', data, 16)
            assert kind == 1 and (data[4], data[5], machine) == IDENTITY[arch]
            record['object_sha256'] = sha(target)
            record['elf'] = dict(type=kind, elf_class=data[4], byte_order=data[5], machine=machine)
            for key in ('compiler', 'cc1plus', 'flag_manifest'):
                assert sha(record[key]) == record[key + '_sha256']
        for name, digest in sources.items():
            assert sha(source / name) == digest, name
        assert sha(xmake) == state['xmake']['sha256']
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print('PASS all ten freestanding dispatcher/completion objects; link=0 guest=0')


if __name__ == '__main__':
    main()
