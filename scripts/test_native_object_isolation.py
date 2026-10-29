#!/usr/bin/env python3
"""Build with xmake, then execute real user reads of supervisor object pages."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time
sys.dont_write_bytecode = True
import test_native_firmware_handoff as firmware

ROOT = Path(__file__).resolve().parents[1]
PROFILES = firmware.profiles
FALLBACK = {'mips', 'mips64', 'loongarch64'}
DONE = b' scope=actual-user-load-supervisor-object\n'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def user_program_layout(nm, elf):
    output = subprocess.check_output([str(nm), '-P', str(elf)], text=True)
    symbols = {}
    for line in output.splitlines():
        parts = line.split()
        if parts and parts[0] in ('ok_object_user_begin', 'ok_object_user_load', 'ok_object_user_end'):
            if len(parts) < 3 or parts[0] in symbols:
                raise ValueError('ambiguous user program symbol')
            symbols[parts[0]] = int(parts[2], 16)
    if len(symbols) != 3:
        raise ValueError('missing user program symbols')
    begin, load, end = (symbols['ok_object_user_'+name] for name in ('begin', 'load', 'end'))
    if not begin <= load < end or not 0 < end-begin <= 4096:
        raise ValueError('invalid retained ELF user program layout')
    return dict(size=end-begin, load_offset=load-begin)


def acceptance(arch, data, layout):
    if arch not in PROFILES or set(layout) != {'size', 'load_offset'} or not 0 <= layout['load_offset'] < layout['size'] <= 4096:
        raise ValueError('invalid architecture or user program layout')
    for line in data.splitlines(keepends=True):
        if line.startswith(b'OBJECT_') and not line.endswith(b'\n'):
            raise ValueError('truncated object evidence')
    lines = data.decode('ascii', errors='strict').splitlines()
    user_begin = 0x10000000 if arch in ('arm32', 'aarch64') else 0x40000000
    user_end = 0x40000000 if arch in ('arm32', 'aarch64') else 0x80000000
    word_limit = 1 << (32 if arch in ('i386', 'arm32', 'rv32', 'mips', 'ppc') else 64)
    range_lines = [line for line in lines if line.startswith('OBJECT_RANGE')]
    def unique(prefix):
        found = [line for line in lines if line.startswith(prefix)]
        if len(found) != 1:
            raise ValueError('missing/duplicate ' + prefix)
        return found[0]
    def fields(line, prefix, names):
        match = re.fullmatch(re.escape(prefix) + ''.join(' '+name+r'=(0x[0-9a-f]{16})' for name in names), line)
        if not match:
            raise ValueError('malformed ' + prefix)
        return dict(zip(names, (int(value, 16) for value in match.groups())))
    done = unique('OBJECT_DONE')
    if done != 'OBJECT_DONE status=0x0000000000000000 scope=actual-user-load-supervisor-object':
        raise ValueError('probe failed: ' + done)
    backing = unique('OBJECT_BACKING')
    if arch in FALLBACK:
        if range_lines: raise ValueError('linker backing cannot contain firmware ranges')
        storage = fields(backing, 'OBJECT_BACKING source=linker', ['base', 'size'])
        if storage['size'] != 16*4096 or storage['base'] % 4096:
            raise ValueError('invalid linker extent')
        ranges = [(storage['base'], storage['size'])]
        total = 16
    else:
        storage = fields(backing, 'OBJECT_BACKING source=firmware', ['pages', 'status'])
        if storage != {'pages': 1024, 'status': 0}:
            raise ValueError('firmware pool unavailable')
        ranges = []
        for line in range_lines:
            value = fields(line, 'OBJECT_RANGE', ['base', 'size'])
            ranges.append((value['base'], value['size']))
        if not ranges or sum(size for _, size in ranges) != 1024*4096:
            raise ValueError('firmware pool size mismatch')
        total = 1024
    ordered_ranges = sorted(ranges)
    if ranges != ordered_ranges: raise ValueError("backing ranges out of order")
    for index, (base, size) in enumerate(ordered_ranges):
        if not size or base % 4096 or size % 4096 or base+size > word_limit:
            raise ValueError('unaligned, empty or wrapping backing range')
        if index and ordered_ranges[index-1][0]+ordered_ranges[index-1][1] > base:
            raise ValueError('overlapping backing ranges')
        if base < user_end and user_begin < base+size:
            raise ValueError('backing overlaps the user virtual window')
    live = fields(unique('OBJECT_LIVE'), 'OBJECT_LIVE', ['address', 'secret', 'objects', 'users'])
    if live['secret'] != 0xc3917a25 or live['objects'] != 1 or live['users'] != 3:
        raise ValueError('real object/provider ownership missing')
    if not any(base <= live['address'] < base+size for base, size in ranges):
        raise ValueError('object outside selected backing')
    cases = []
    for name in ['before', 'protected', 'after']:
        case = fields(unique('OBJECT_CASE name='+name), 'OBJECT_CASE name='+name,
                      ['pointer', 'kind', 'vector', 'error', 'fault', 'pc', 'load_pc', 'observed', 'completed', 'status'])
        if case['load_pc'] != user_begin+layout['load_offset'] or not user_begin <= case['pc'] < user_begin+layout['size']:
            raise ValueError('trap PC differs from the retained ELF user program')
        if case['status']:
            raise ValueError('case failed')
        if name == 'protected':
            if case['kind'] != 2 or case['pointer'] != live['address'] or case['fault'] != live['address'] or case['pc'] != case['load_pc'] or case['observed'] != 0xa5b6c7d8 or case['completed']:
                raise ValueError('exact protected load did not fault')
            expected = {'i386': {14}, 'x86_64': {14}, 'aarch64': {0x24}, 'arm32': {4}, 'rv64': {13}, 'rv32': {13}, 'mips': {4}, 'mips64': {4}, 'ppc': {2, 13}, 'loongarch64': {0, 1, 7, 8}}
            if case['vector'] not in expected[arch]:
                raise ValueError('wrong architecture load fault')
            if arch in ('i386', 'x86_64') and case['error'] & 0x1f != 5:
                raise ValueError('not a user read protection fault')
            if arch == 'aarch64' and (case['error'] & (1 << 6) or case['error'] & 0x3c != 0xc):
                raise ValueError('not a lower EL read permission fault')
        elif case['kind'] != 0 or case['observed'] != 0x12345678 or case['completed'] != 1 or case['pointer'] != user_begin+4096+64 or case['vector'] != {'i386':128, 'x86_64':256, 'aarch64':21, 'arm32':2, 'rv32':8, 'rv64':8, 'ppc':8, 'mips':8, 'mips64':8, 'loongarch64':11}[arch]:
            raise ValueError('positive control failed')
        cases.append(case)
    reclaim = fields(unique('OBJECT_RECLAIM'), 'OBJECT_RECLAIM', ['retired', 'free', 'total', 'objects', 'users', 'allocations', 'releases'])
    if reclaim != dict(retired=1, free=total, total=total, objects=0, users=0, allocations=4, releases=4):
        raise ValueError('retirement/accounting failure')
    ordered = [backing, *range_lines, unique('OBJECT_LIVE'), *(unique('OBJECT_CASE name='+name) for name in ['before','protected','after']), unique('OBJECT_RECLAIM'), done]
    if [lines.index(line) for line in ordered] != sorted(lines.index(line) for line in ordered):
        raise ValueError('evidence order invalid')
    allowed = ('OBJECT_BACKING', 'OBJECT_RANGE', 'OBJECT_LIVE', 'OBJECT_CASE', 'OBJECT_RECLAIM', 'OBJECT_DONE')
    if any(line.startswith('OBJECT_') and not line.startswith(allowed) for line in lines):
        raise ValueError('unknown object evidence')
    if len([line for line in lines if line.startswith('OBJECT_CASE')]) != 3:
        raise ValueError('unexpected case')
    return {'backing': 'linker' if arch in FALLBACK else 'qualified-firmware', 'ranges': ranges, 'live': live, 'cases': cases, 'reclaim': reclaim}

def lua_args(values):
    return ', '.join(json.dumps(str(value)) for value in values)

def project(snap, arch, cfg, tools):
    flags = cfg['cxxflags'] + ['-fno-builtin', '-Werror', '-ffunction-sections', '-fdata-sections']
    window = firmware.windows.get(arch, (0,0))
    defines = cfg['defines'] + [f'OK_PROBE_ARCH={arch}', f'OK_PROBE_PROTOCOL={firmware.protocols[arch]}', f'OK_PROBE_WINDOW_START={window[0]}ULL', f'OK_PROBE_WINDOW_END={window[1]}ULL']
    sources = ['probe.cpp', 'user.S', 'src/boot/raw_handoff.cpp', 'src/boot/fdt.cpp', 'src/memory/memory.cpp', f'src/arch/{arch}/boot.S', f'src/arch/{arch}/execution.cpp', 'src/interrupt/platform_irq.cpp', f'src/arch/{arch}/execution.S']
    code = '\n'.join([
        'set_project("object-isolation")', 'set_languages("c++23")',
        'toolchain("probe-cross")', '    set_kind("standalone")',
        f'    set_toolset("cc", {json.dumps(str(tools["gcc"]))})',
        f'    set_toolset("cxx", {json.dumps(str(tools["g++"]))})',
        f'    set_toolset("as", {json.dumps(str(tools["gcc"]))})',
        f'    set_toolset("ld", {json.dumps(str(tools["g++"]))})',
        'toolchain_end()', 'target("probe")', '    set_kind("binary")',
        '    set_toolchains("probe-cross")', '    set_filename("probe.elf")',
        '    set_targetdir('+json.dumps(str(snap/'artifacts'))+')', '    add_includedirs("include")',
        '    add_files('+lua_args(sources)+')',
        '    add_defines('+lua_args(defines)+')',
        '    add_cxxflags('+lua_args(flags)+', {force=true})',
        '    add_asflags('+lua_args(cfg['asflags'])+', {force=true})',
        '    add_ldflags('+lua_args(cfg['ldflags']+['-nostdlib','-static','-Wl,--gc-sections','-Wl,--build-id=none','-Wl,-T,'+str(snap/f'src/arch/{arch}/linker.ld')])+', {force=true})',
        '    add_links("gcc")', '    after_build(function (target)',
    ])
    if cfg['image_format'] == 'elf':
        code += '\n        os.cp(target:targetfile(), "artifacts/kernel.bin")'
    else:
        code += '\n        os.execv('+json.dumps(str(tools['objcopy']))+', {"-O", "binary", target:targetfile(), "artifacts/payload.bin"})'
        if arch in ('i386','x86_64'):
            code += '\n        os.execv('+json.dumps(str(tools['gcc']))+', {"-m32", "-c", "src/arch/x86_64/boot16.S", "-o", "artifacts/boot16.o"})'
            code += '\n        os.execv('+json.dumps(str(tools['ld']))+', {"-m", "elf_i386", "-Ttext", "0x7c00", "--oformat", "binary", "artifacts/boot16.o", "-o", "artifacts/boot16.bin"})'
        else:
            code += '\n        os.cp("artifacts/payload.bin", "artifacts/kernel.bin")'
    return code+'\n    end)\ntarget_end()\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-root',type=Path,default=ROOT)
    parser.add_argument('--toolchains',type=Path,required=True)
    parser.add_argument('--qemu-dir',type=Path,default=Path('/usr/bin'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--arch',choices=list(PROFILES),action='append')
    args=parser.parse_args()
    root=args.kernel_root.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    xmake=Path(shutil.which('xmake')).resolve();rows=[]
    for arch in args.arch or PROFILES:
        row={'arch':arch,'status':'running','scope':'actual QEMU user load, exact supervisor-object fault, positive recovery and page retirement; serialized boot CPU only'}
        rows.append(row);directory=out/arch;directory.mkdir();snap=directory/'sources';snap.mkdir()
        cfg=PROFILES[arch];prefix=args.toolchains.resolve()/cfg['triple']/'bin'/cfg['triple']
        tools={name:Path(str(prefix)+'-'+name) for name in ['gcc','g++','ld','objcopy','nm']}
        sources=['tests/native/object_isolation_probe.cpp','tests/native/object_isolation_user.S','src/boot/raw_handoff.cpp','src/boot/fdt.cpp','src/memory/memory.cpp',f'src/arch/{arch}/boot.S',f'src/arch/{arch}/linker.ld',f'src/arch/{arch}/execution.cpp','src/interrupt/platform_irq.cpp','src/interrupt/pic8259.hpp','src/arch/arm_common/gicv2.hpp','src/arch/riscv_common/plic.hpp','src/arch/loongarch64/eiointc.hpp','src/arch/ppc/mpic.hpp',f'src/arch/{arch}/execution.S']
        sources += [str(path.relative_to(root)) for path in (root/'include').rglob('*.hpp')]
        if arch in ('mips','mips64'):
            sources += [str(path.relative_to(root)) for path in (root/'src/arch/mips_common').iterdir() if path.is_file()]
        if arch in ('i386','x86_64'): sources += ['src/arch/x86_64/boot16.S']
        sources += ['scripts/test_native_object_isolation.py','scripts/test_native_firmware_handoff.py','scripts/native_firmware_profiles.json']
        for relative in sources:
            target=snap/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/relative,target)
        shutil.copyfile(snap/'tests/native/object_isolation_probe.cpp',snap/'probe.cpp')
        shutil.copyfile(snap/'tests/native/object_isolation_user.S',snap/'user.S')
        (snap/'xmake.lua').write_text(project(snap,arch,cfg,tools))
        row['source_sha256']={str(p.relative_to(snap)):digest(p) for p in sorted(snap.rglob('*')) if p.is_file()}
        row['tools_sha256']={str(path):digest(path) for path in [*tools.values(),xmake]}
        row['commands']=[]
        try:
            for argument,expected in [('-dumpmachine',cfg['triple']),('-dumpfullversion',cfg['compiler']['version'])]:
                if subprocess.check_output([tools['gcc'],argument],text=True).strip()!=expected: raise ValueError('compiler identity mismatch')
            for label,command in [('configure',[str(xmake),'f','-P',str(snap),'-c','-y','-o',str(snap/'build'),'--ccache=n']),('build',[str(xmake),'-P',str(snap),'-v','-y','-j','2'])]:
                with (directory/(label+'.log')).open('w') as log:
                    result=subprocess.run(command,cwd=snap,stdout=log,stderr=subprocess.STDOUT)
                row['commands'].append({'argv':command,'returncode':result.returncode})
                if result.returncode:raise ValueError(label+' failed')
            artifacts=snap/'artifacts'
            if arch in ('i386','x86_64'):
                boot=(artifacts/'boot16.bin').read_bytes();payload=(artifacts/'payload.bin').read_bytes()
                if len(boot)!=512 or boot[-2:]!=b'\x55\xaa' or len(payload)>8*1024*1024:raise ValueError('BIOS image extent invalid')
                (artifacts/'kernel.bin').write_bytes(boot+payload+bytes(8*1024*1024-len(payload)))
            qemu=args.qemu_dir.resolve()/('qemu-system-'+{'arm32':'arm','rv64':'riscv64','rv32':'riscv32'}.get(arch,arch))
            command=firmware.qemu_command(qemu,arch,artifacts/'kernel.bin');row['qemu']=command;row['qemu_sha256']=digest(qemu)
            child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            data=bytearray();deadline=time.monotonic()+30
            try:
                while time.monotonic()<deadline and DONE not in data:
                    if select.select([child.stdout],[],[],.1)[0]:
                        chunk=os.read(child.stdout.fileno(),8192)
                        if not chunk:break
                        data+=chunk
            finally:
                child.terminate()
                try:data+=child.communicate(timeout=2)[0]
                except subprocess.TimeoutExpired:child.kill();data+=child.communicate()[0]
            (directory/'serial.log').write_bytes(data)
            row['user_program_layout']=user_program_layout(tools['nm'],artifacts/'probe.elf')
            row['facts']=acceptance(arch,data,row['user_program_layout'])
            row['status']='pass'
        except Exception as error:
            row.update(status='fail',failure=str(error))
        row['artifacts_sha256']={str(path.relative_to(directory)):digest(path) for path in sorted(directory.rglob('*')) if path.is_file() and (path.suffix=='.log' or path.parent==snap/'artifacts')}
        for relative,sha in row['source_sha256'].items():
            if digest(snap/relative)!=sha:row.update(status='fail',failure='source changed during execution')
        (directory/'result.json').write_text(json.dumps(row,indent=2)+'\n')
        (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(arch,row['status'],row.get('failure',row.get('facts')),flush=True)
    return int(any(row['status']!='pass' for row in rows))
if __name__=='__main__':raise SystemExit(main())
