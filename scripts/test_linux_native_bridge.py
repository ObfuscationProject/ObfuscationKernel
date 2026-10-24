#!/usr/bin/env python3
"""Audit saved-frame anchors; run all-ten bridge models and optional isolated cross-compiles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tarfile
import tempfile

PROFILES = (
    ('i386', 'i386-elf', 1, 1, 3), ('x86_64', 'x86_64-elf', 2, 1, 62),
    ('aarch64', 'aarch64-elf', 2, 1, 183), ('arm32', 'arm-none-eabi', 1, 1, 40),
    ('rv64', 'riscv64-elf', 2, 1, 243), ('rv32', 'riscv32-elf', 1, 1, 243),
    ('loongarch64', 'loongarch64-elf', 2, 1, 258), ('mips', 'mips-elf', 1, 2, 8),
    ('mips64', 'mips64-elf', 2, 2, 8), ('ppc', 'powerpc-eabi', 1, 2, 20),
)
TARGET_FLAGS = {
    'i386': ['-mno-sse', '-mno-sse2'],
    'x86_64': ['-mno-red-zone'],
    'aarch64': ['-mno-outline-atomics', '-mgeneral-regs-only'],
    'arm32': ['-march=armv7-a', '-marm'],
    'rv64': ['-mcmodel=medany'],
    'rv32': ['-mcmodel=medany'],
    'loongarch64': [],
    'mips': ['-march=mips32r2', '-G0'],
    'mips64': ['-march=mips64r2', '-mabi=64', '-G0'],
    'ppc': ['-msoft-float', '-msdata=none'],
}

# Review anchors guard the private frame contract against silent layout drift.
# They are source audits, not evidence that instructions executed.
ANCHORS = {
    'i386/execution.cpp': ['constexpr usize eax_slot = 7;', 'constexpr usize ebx_slot = 4;',
                          'constexpr usize ecx_slot = 6;', 'constexpr usize edx_slot = 5;',
                          'constexpr usize esi_slot = 1;', 'constexpr usize edi_slot = 0;',
                          'constexpr usize ebp_slot = 2;', 'gdt[7] = data_descriptor(frame.gs_base);'],
    'i386/execution.S': ['mov 32(%eax), %gs', 'mov %ss:28(%eax), %eax', 'pushal', 'pushl FRAME_SP(%eax)'],
    'x86_64/execution.cpp': ['constexpr usize rax_slot = 14;', 'constexpr usize rdi_slot = 9;',
                            'constexpr usize rsi_slot = 10;', 'constexpr usize rdx_slot = 11;',
                            'constexpr usize r10_slot = 5;', 'constexpr usize r8_slot = 7;',
                            'constexpr usize r9_slot = 6;', 'write_msr(0xc0000100, frame.fs_base);'],
    'x86_64/execution.S': ['mov 0(%rax), %r15', 'mov 112(%rax), %rax', 'pushq FRAME_SP(%rdi)',
                         'mov 96(%rax), %rcx', 'mov 104(%rax), %rbx'],
    'aarch64/execution.S': ['ldr x8, [x9, #64]', 'ldp x4, x5, [x9, #32]', 'ldr x1, [x9, #248]',
                          'ldp x1, x2, [x9, #280]', 'msr tpidr_el0, x1', 'str x2, [x1, #248]'],
    'arm32/execution.S': ['ldmia r4, {r0-r12}', 'ldr r3, [r4, #144]', 'mcr p15, 0, r3, c13, c0, 3',
                         'mrc p15, 0, r5, c13, c0, 3', 'str r5, [r4, #144]'],
    'rv64/execution.S': ['ld x4, 32(t6)', 'ld x17, 136(t6)', 'ld x15, 120(t6)', 'ld sp, 264(t6)',
                        'sd x4, 32(t0)', 'sd x10, 80(t0)'],
    'rv32/execution.S': ['lw x4, 16(t6)', 'lw x17, 68(t6)', 'lw x15, 60(t6)', 'lw sp, 132(t6)',
                        'sw x4, 16(t0)', 'sw x10, 40(t0)'],
    'loongarch64/execution.S': ['ld.d $r2, $t0, 16', 'ld.d $r3, $t0, 264', 'ld.d $r11, $t0, 88',
                              'ld.d $r9, $t0, 72', 'st.d $r2, $t0, 16'],
    'mips/execution.S': ['#include "../mips_common/execution_asm.h"'],
    'mips64/execution.S': ['#include "../mips_common/execution_asm.h"'],
    'mips_common/execution_asm.h': ['#define PTR 8', '#define PTR 4', 'LOAD $2, 2*PTR($26)',
                                  'LOAD $7, 7*PTR($26)', 'LOAD $9, 9*PTR($26)', 'LOAD $29, 33*PTR($26)',
                                  'STORE $2, 2*PTR($26)', 'STORE $7, 7*PTR($26)', 'STORE $29, 33*PTR($26)'],
    'mips_common/execution_impl.hpp': ['(insn & 0xffe0ffffu) == 0x7c00e83bu',
                                     'frame.registers[destination] = frame.fs_base;'],
    'ppc/execution.S': ['lwz 0,440(12)', 'mtcr 0', 'stw 0,440(12)', 'lwz 0,136(12)',
                        'lwz 0,140(12)', 'stw 0,8(12)', 'lwz 0,144(12)', 'stw 0,52(12)',
                        'lwz 2,8(12)', 'lwz 3,12(12)', 'lwz 8,32(12)', 'stw 2,140(12)'],
}


def audit_sources(root):
    for name, fragments in ANCHORS.items():
        content = ''.join((root / 'src/arch' / name).read_text().split())
        for fragment in fragments:
            if ''.join(fragment.split()) not in content:
                raise ValueError(f'native frame contract changed: {name}: {fragment}')
    print('PASS source frame/restore anchors for all ten profiles (static audit, no CPU execution)', flush=True)


def audit_musl(root, path):
    manifest = json.loads((root / 'docs/abi/linux-profiles.json').read_text())
    if hashlib.sha256(path.read_bytes()).hexdigest() != manifest['musl_archive_sha256']:
        raise ValueError('musl source archive differs from the qualified pin')
    with tarfile.open(path, 'r:gz') as archive:
        for profile in manifest['profiles']:
            name = 'arch/' + profile['musl_port'] + '/syscall_arch.h'
            source = archive.extractfile('musl-' + manifest['musl_version'] + '/' + name).read()
            if hashlib.sha256(source).hexdigest() != manifest['source_file_sha256']['musl/' + name]:
                raise ValueError('musl syscall source differs from codec audit: ' + name)
    print('PASS exact pinned musl syscall_arch.h source identities for all ten profiles', flush=True)


def cross_compile(root, toolchains, work):
    for name, triple, elf_class, byte_order, machine in PROFILES:
        compiler = toolchains / triple / 'bin' / (triple + '-g++')
        if not compiler.is_file():
            raise ValueError('explicit target compiler is unavailable: ' + str(compiler))
        output = work / (name + '.o')
        command = [str(compiler), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                   '-fno-exceptions', '-fno-threadsafe-statics', *TARGET_FLAGS[name], '-DOK_ARCH_TARGET_' + name.upper(),
                   '-I' + str(root / 'include'), '-c', str(root / 'tests/host/linux_native_bridge_compile.cpp'),
                   '-o', str(output)]
        subprocess.run(command, check=True)
        data = output.read_bytes()
        order = '<' if byte_order == 1 else '>'
        if data[:4] != b'\x7fELF' or data[4:6] != bytes((elf_class, byte_order)) or struct.unpack_from(order + 'H', data, 18)[0] != machine:
            raise ValueError('wrong isolated object ABI: ' + str(output))
        print(f'PASS {name}: isolated freestanding compile and ELF class/endian/machine (not executed)', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cross', action='store_true')
    parser.add_argument('--toolchain-root', type=Path)
    parser.add_argument('--musl-archive', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    audit_sources(root)
    if args.musl_archive:
        audit_musl(root, args.musl_archive)
    with tempfile.TemporaryDirectory(prefix='ok-linux-native-bridge-') as directory:
        work = Path(directory)
        output = work / 'host-tests'
        command = [*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                   '-fno-exceptions', '-I' + str(root / 'include'), str(root / 'tests/host/linux_native_bridge_tests.cpp'),
                   '-o', str(output)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(output)], check=True)
        if args.cross:
            cross_compile(root, args.toolchain_root or root / 'toolchains', work)


if __name__ == '__main__':
    main()
