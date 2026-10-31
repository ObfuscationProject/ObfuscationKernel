#!/usr/bin/env python3
"""Hosted net-core model tests and all-ten freestanding xmake object checks.

Covers the family-generic wire layer (IPv6, ICMP, neighbour discovery) and the
protocol models built on it. No NIC, user execution or hardware is qualified here.
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
    'aarch64': ['-mno-outline-atomics', '-mgeneral-regs-only', '-mstrict-align'],
    'arm32': ['-march=armv7-a', '-marm', '-mno-unaligned-access'], 'rv64': ['-mcmodel=medany'],
    'rv32': ['-mcmodel=medany'], 'loongarch64': [],
    'mips': ['-march=mips32r2', '-G0'], 'mips64': ['-march=mips64r2', '-mabi=64', '-G0'],
    'ppc': ['-msoft-float', '-msdata=none'],
}
# Each hosted model test is its own binary with its own main.
TESTS = ('net_ip_tests', 'net_neighbour_route_tests', 'net_interface_tests', 'tcp_window_tests')
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
    for name in (*(test + '.cpp' for test in TESTS), 'net_core_compile.cpp'):
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
        lines = ['set_project("net-core-hosted")', 'set_languages("cxx23")']
        for test in TESTS:
            lines += ['target(' + json.dumps(test) + ')', 'set_kind("binary")', 'set_targetdir("bin")',
                      'add_includedirs("include")', 'add_defines("OK_ARCH_TARGET_X86_64")',
                      'add_files("tests/host/' + test + '.cpp")',
                      'add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror",{force=true})']
            if a.sanitize:
                lines += ['add_cxxflags("-fsanitize=address,undefined","-fno-sanitize-recover=all",'
                          '"-fno-omit-frame-pointer",{force=true})',
                          'add_ldflags("-fsanitize=address,undefined",{force=true})']
            lines.append('target_end()')
        (source / 'xmake.lua').write_text('\n'.join(lines) + '\n')
        result['host_project_sha256'] = sha(source / 'xmake.lua')
        run([xmake, 'f', '-P', source, '-c', '-y', '--ccache=n', '--cxx='+str(cxx), '--ld='+str(cxx)], 'host-configure')
        run([xmake, 'build', '-P', source, '-a', '-j4', '-v'], 'host-build')
        result['host_output'] = {}
        for test in TESTS:
            binary = source / 'bin' / test
            result['host_binary_sha256_' + test] = sha(binary)
            text = run([binary], 'host-run-' + test); result['host_output'][test] = text
            print(text, end='', flush=True)
        if a.toolchain_root:
            for arch, triple, elf_class, endian, machine in PROFILES:
                compiler = pin(a.toolchain_root / triple / 'bin' / (triple + '-g++'))
                pin(subprocess.check_output([compiler, '-print-prog-name=cc1plus'], text=True).strip())
                work = out / arch; work.mkdir()
                flags = ['-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-ffreestanding',
                         '-fno-exceptions', '-frtti', '-fno-stack-protector', '-fno-use-cxa-atexit',
                         '-fno-threadsafe-statics', *FLAGS[arch]]
                lines = ['set_project("net-core-target")', 'set_languages("cxx23")',
                         'toolchain("net-core-target")', 'set_kind("standalone")',
                         'set_toolset("cxx",'+json.dumps(str(compiler))+')', 'toolchain_end()',
                         'target("net-core-object")', 'set_kind("object")', 'set_toolchains("net-core-target")',
                         'add_defines("OK_KERNEL_FREESTANDING","OK_ARCH_TARGET_'+arch.upper()+'")',
                         'add_includedirs('+json.dumps(str(source / 'include'))+')',
                         'add_files('+json.dumps(str(source / 'tests/host/net_core_compile.cpp'))+')',
                         'add_cxxflags('+','.join(map(json.dumps, flags))+',{force=true})', 'target_end()']
                (work / 'xmake.lua').write_text('\n'.join(lines) + '\n')
                run([xmake, 'f', '-P', work, '-c', '-y', '--ccache=n'], arch+'-configure', work)
                run([xmake, 'build', '-P', work, '-j2', '-v'], arch+'-build', work)
                # xmake owns this isolated one-source object directory. Count and
                # ABI validation prevent selecting an arbitrary glob match.
                objects = list((work / 'build/.objs/net-core-object').rglob('*.o'))
                assert len(objects) == 1
                obj = objects[0]; data = obj.read_bytes()
                assert data[:4] == b'\x7fELF' and data[4:6] == bytes((elf_class, endian))
                assert struct.unpack_from('<H' if endian == 1 else '>H', data, 18)[0] == machine
                result['profiles'][arch] = dict(object=str(obj.relative_to(out)), sha256=sha(obj),
                                                project_sha256=sha(work / 'xmake.lua'))
                print('PASS freestanding net-core object', arch, '(not executed)', flush=True)
        for name, digest in inputs.items(): assert sha(source / name) == digest
        result['status'] = 'passed'
    except BaseException as error:
        result.update(status='failed', error=repr(error)); save(); raise
    save()

if __name__ == '__main__': main()
