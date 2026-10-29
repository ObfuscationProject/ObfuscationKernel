#!/usr/bin/env python3
"""Compile emitted native mount-info C/C++ wire and actual marshaller for all twenty tuples."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
from test_ext4_owned import PROFILES, TARGET_FLAGS


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(data, name):
    """Read actual emitted ELF bytes, independently of source spellings."""
    endian = '<' if data[5] == 1 else '>'
    wide = data[4] == 2
    offset = struct.unpack_from(endian + ('Q' if wide else 'I'), data, 40 if wide else 32)[0]
    entry, count, names = struct.unpack_from(endian + 'HHH', data, 58 if wide else 46)
    def fields(index):
        base = offset + index * entry
        return (struct.unpack_from(endian + 'I', data, base)[0],
                *struct.unpack_from(endian + ('QQ' if wide else 'II'), data, base + (24 if wide else 16)))
    _, start, size = fields(names)
    strings = data[start:start + size]
    for index in range(count):
        label, start, size = fields(index)
        if strings[label:strings.index(b'\0', label)] == name.encode():
            return data[start:start + size]
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain-root', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    paths = ['tests/native/native_mount_info_wire_compile.c', 'tests/native/native_mount_info_wire_compile.cpp']
    report = dict(status='preparing', scope='All-twenty native C/C++ fixed wire and actual marshaller compile only; no syscall execution, guest, terminal or physical execution.',
                  commands=[], rows=[], source_sha256={}, tools={})
    def save():
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    def run(argv, label):
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=source, env=dict(os.environ, XMAKE_ROOT='y'),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = out / (label + '.log'); log.write_bytes(result.stdout)
        report['commands'].append(dict(argv=argv, returncode=result.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if result.returncode:
            raise RuntimeError(label + ' failed; retained ' + str(log))
    save()
    try:
        files = {root / p for p in paths} | {Path(__file__).resolve(), root / 'scripts/test_ext4_owned.py'}
        files.update(p for p in (root / 'include').rglob('*') if p.is_file())
        for path in sorted(files):
            name = str(path.relative_to(root)); target = source / name
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
            report['source_sha256'][name] = sha(target)
            if sha(target) != sha(path): raise ValueError('snapshot changed: ' + name)
        lua = ['set_project("native-mount-wire-cross")', 'set_languages("c11", "c++23")']
        for arch, triple, *_ in PROFILES:
            prefix = args.toolchain_root.resolve() / triple / 'bin' / (triple + '-')
            cc, cxx, ar = (Path(str(prefix) + name) for name in ('gcc', 'g++', 'ar'))
            for tool in (cc, cxx, ar):
                if not tool.is_file(): raise ValueError('missing explicit toolchain: ' + str(tool))
                report['tools'][str(tool)] = sha(tool)
            flags = TARGET_FLAGS[arch] + (['-msimd=none'] if arch == 'loongarch64' else [])
            flags += ['-ffreestanding', '-fno-stack-protector', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
            lua += ['toolchain("wire-' + arch + '")', 'set_kind("standalone")',
                    'set_toolset("cc", ' + json.dumps(str(cc)) + ')', 'set_toolset("cxx", ' + json.dumps(str(cxx)) + ')',
                    'set_toolset("ar", ' + json.dumps(str(ar)) + ')', 'toolchain_end()']
            for mode in ('debug', 'release'):
                definitions = ['OK_ARCH_TARGET_' + arch.upper(), 'OK_KERNEL_FREESTANDING', 'OK_NATIVE_SIGNAL_RUNTIME']
                if mode == 'debug': definitions.append('OK_ENABLE_TEST_POINTS')
                selected = flags + ['-O2'] + (['-g'] if mode == 'debug' else [])
                lua += ['target("wire-' + arch + '-' + mode + '")', 'set_kind("static")',
                        'set_toolchains("wire-' + arch + '")', 'add_includedirs("include")', 'set_targetdir("lib")',
                        'add_defines(' + ', '.join(map(json.dumps, definitions)) + ')',
                        'add_cflags(' + ', '.join(map(json.dumps, selected)) + ', {force=true})',
                        'add_cxxflags(' + ', '.join(map(json.dumps, selected + ['-fno-exceptions', '-frtti', '-fno-threadsafe-statics'])) + ', {force=true})',
                        'add_files(' + ', '.join(map(json.dumps, paths)) + ')', 'target_end()']
        (source / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        report['generated_project_sha256'] = sha(source / 'xmake.lua'); report['status'] = 'running'; save()
        run(['xmake', 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        run(['xmake', 'build', '-P', source, '-j4', '-v'], 'all-ten-build')
        for arch, _, elf_class, endian, machine in PROFILES:
            for mode in ('debug', 'release'):
                archive = source / 'lib' / ('libwire-' + arch + '-' + mode + '.a')
                data = archive.read_bytes(); offset = 8; objects = 0; wires = {}
                if data[:8] != b'!<arch>\n': raise ValueError('invalid native archive: ' + arch)
                while offset < len(data):
                    header = data[offset:offset + 60]
                    if len(header) != 60 or header[58:] != b'`\n': raise ValueError('invalid archive member')
                    size = int(header[48:58]); body = data[offset + 60:offset + 60 + size]
                    if body[:4] == b'\x7fELF':
                        objects += 1
                        actual = [body[4], body[5], int.from_bytes(body[18:20], 'little' if endian == 1 else 'big')]
                        if actual != [elf_class, endian, machine]: raise ValueError('wrong native ABI: ' + arch)
                        for name in ('.ok_mount_wire_c', '.ok_mount_wire_cpp'):
                            emitted = section(body, name)
                            if emitted is not None:
                                if name in wires: raise ValueError('duplicate emitted wire section')
                                wires[name] = list(struct.unpack(('<' if endian == 1 else '>') + '30I', emitted))
                    offset += 60 + size + size % 2
                expected = [0, 16, 0, 1, 1034, 288, 0, 4, 8, 12, 16, 20, 24, 28, 32,
                            9, 256, 0, 1, 2, 3, 4, 5, 4, 4, 32 if elf_class == 1 else 64,
                            0xffffffff, 0xffffffea, 0xffffffda, 0xfffffff2]
                if offset != len(data) or objects != 2 or wires != {name: expected for name in ('.ok_mount_wire_c', '.ok_mount_wire_cpp')}:
                    raise ValueError('native mount wire bytes differ: ' + arch)
                report['rows'].append(dict(arch=arch, mode=mode, status='passed', archive_sha256=sha(archive), emitted_wire=wires))
                save(); print('PASS native mount wire', arch, mode, '(compile only)', flush=True)
        for name, digest in report['source_sha256'].items():
            if sha(source / name) != digest: raise ValueError('retained source changed: ' + name)
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=repr(error)); save(); raise
    save()


if __name__ == '__main__':
    main()
