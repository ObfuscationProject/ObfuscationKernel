#!/usr/bin/env python3
"""Hosted network byte tests and all-ten freestanding xmake object checks.

No NIC, protocol engine, sockets, user execution or hardware is qualified here.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

PROFILES = (
    ('i386', 'i386-elf', 1, 1, 3), ('x86_64', 'x86_64-elf', 2, 1, 62),
    ('aarch64', 'aarch64-elf', 2, 1, 183), ('arm32', 'arm-none-eabi', 1, 1, 40),
    ('rv64', 'riscv64-elf', 2, 1, 243), ('rv32', 'riscv32-elf', 1, 1, 243),
    ('loongarch64', 'loongarch64-elf', 2, 1, 258), ('mips', 'mips-elf', 1, 2, 8),
    ('mips64', 'mips64-elf', 2, 2, 8), ('ppc', 'powerpc-eabi', 1, 2, 20),
)
FLAGS = {
    'i386': ['-mno-sse', '-mno-sse2'], 'x86_64': ['-mno-red-zone'],
    'aarch64': ['-mno-outline-atomics', '-mgeneral-regs-only'],
    'arm32': ['-march=armv7-a', '-marm'], 'rv64': ['-mcmodel=medany'],
    'rv32': ['-mcmodel=medany'], 'loongarch64': [],
    'mips': ['-march=mips32r2', '-G0'], 'mips64': ['-march=mips64r2', '-mabi=64', '-G0'],
    'ppc': ['-msoft-float', '-msdata=none'],
}
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--toolchain-root', type=Path)
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    assert __debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1'
    root = Path(__file__).resolve().parents[1]
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'; source.mkdir()
    shutil.copytree(root / 'include', source / 'include')
    for name in ('network_wire_tests.cpp', 'network_wire_compile.cpp'):
        (source / 'tests/host').mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / 'tests/host' / name, source / 'tests/host' / name)
    inputs = {str(f.relative_to(source)): sha(f) for f in sorted(source.rglob('*')) if f.is_file()}
    result = dict(status='running', scope='hosted byte/model tests and freestanding objects only',
                  sanitized=a.sanitize, source_files=inputs, script_sha256=sha(__file__),
                  commands=[], profiles={}, tools={}, hardware=False, native_execution=False)
    def save(): (out / 'result.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    def pin(path):
        path = Path(path).resolve(); result['tools'][str(path)] = sha(path); return path
    def run(argv, label, cwd=source):
        argv = list(map(str, argv)); log = out / (label + '.log')
        env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out / 'xmake-global'),
                   PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(key, None)
        with log.open('xb') as stream:
            done = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        result['commands'].append(dict(argv=argv, cwd=str(cwd), returncode=done.returncode,
                                        log=log.name, log_sha256=sha(log)))
        save()
        if done.returncode: raise RuntimeError(str(log))
        return log.read_text()
    save()
    try:
        xmake = pin(shutil.which('xmake')); cxx = pin('/usr/bin/g++')
        pin(subprocess.check_output([cxx, '-print-prog-name=cc1plus'], text=True).strip())
        lines = ['set_project("network-wire-hosted")', 'set_languages("cxx23")',
                 'target("network-wire")', 'set_kind("binary")', 'set_targetdir("bin")',
                 'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
                 'add_files("tests/host/network_wire_tests.cpp")',
                 'add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror",{force=true})']
        if a.sanitize:
            lines += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                      'add_ldflags("-fsanitize=address,undefined",{force=true})']
        (source / 'xmake.lua').write_text('\n'.join(lines + ['target_end()']) + '\n')
        result['host_project_sha256'] = sha(source / 'xmake.lua')
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n', '--cxx='+str(cxx), '--ld='+str(cxx)], 'host-configure')
        run([xmake, 'build', '-P', source, '-j2', '-v'], 'host-build')
        binary = source / 'bin/network-wire'
        result['host_binary_sha256'] = sha(binary)
        text = run([binary], 'host-run'); result['host_output'] = text
        print(text, end='', flush=True)
        if a.toolchain_root:
            for arch, triple, elf_class, endian, machine in PROFILES:
                compiler = pin(a.toolchain_root / triple / 'bin' / (triple + '-g++'))
                pin(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip())
                work = out / arch; work.mkdir()
                flags = ['-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-ffreestanding',
                         '-fno-exceptions', '-frtti', '-fno-stack-protector', '-fno-use-cxa-atexit',
                         '-fno-threadsafe-statics', *FLAGS[arch]]
                lines = ['set_project("network-wire-target")', 'set_languages("cxx23")',
                         'toolchain("wire-target")', 'set_kind("standalone")',
                         'set_toolset("cxx",'+json.dumps(str(compiler))+')', 'toolchain_end()',
                         'target("network-wire-object")', 'set_kind("object")', 'set_toolchains("wire-target")',
                         'add_defines("OK_KERNEL_FREESTANDING","OK_ARCH_TARGET_'+arch.upper()+'")',
                         'add_includedirs('+json.dumps(str(source / 'include'))+')',
                         'add_files('+json.dumps(str(source / 'tests/host/network_wire_compile.cpp'))+')',
                         'add_cxxflags('+','.join(map(json.dumps, flags))+',{force=true})', 'target_end()']
                (work / 'xmake.lua').write_text('\n'.join(lines) + '\n')
                run([xmake, 'f', '-P', work, '-c', '-y', '--ccache=n'], arch+'-configure', work)
                run([xmake, 'build', '-P', work, '-j2', '-v'], arch+'-build', work)
                # xmake owns this isolated one-source object directory. Count and
                # ABI validation prevent selecting an arbitrary glob match.
                objects = list((work / 'build/.objs/network-wire-object').rglob('*.o'))
                assert len(objects) == 1
                obj = objects[0]; data = obj.read_bytes()
                assert data[:4] == b'\x7fELF' and data[4:6] == bytes((elf_class, endian))
                assert struct.unpack_from('<H' if endian == 1 else '>H', data, 18)[0] == machine
                result['profiles'][arch] = dict(object=str(obj.relative_to(out)), sha256=sha(obj),
                                                project_sha256=sha(work / 'xmake.lua'))
                print('PASS freestanding network wire object', arch, '(not executed)', flush=True)
        for name, digest in inputs.items(): assert sha(source / name) == digest
        result['status'] = 'passed'
    except BaseException as error:
        result.update(status='failed', error=repr(error)); save(); raise
    save()

if __name__ == '__main__': main()
