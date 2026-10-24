#!/usr/bin/env python3
"""Actual QEMU qualified frame ownership, native mappings and userspace stores; process allocator unchanged."""
from pathlib import Path
import argparse, hashlib, json, os, re, select, struct, subprocess, sys, time
sys.dont_write_bytecode=True
import test_native_firmware_handoff as firmware
stage=Path(__file__).resolve().parents[1]
profiles=firmware.profiles
protocols=firmware.protocols
windows=firmware.windows
supported=firmware.supported
digest=firmware.digest
qemu_command=firmware.qemu_command

def loaded_elf_extents(data):
    if len(data)<16 or data[:4]!=b'\x7fELF' or data[4] not in (1,2) or data[5] not in (1,2):raise ValueError('invalid ELF identity')
    endian='<' if data[5]==1 else '>'
    header=struct.Struct(endian+('HHIIIIIHHHHHH' if data[4]==1 else 'HHIQQQIHHHHHH'))
    if len(data)<16+header.size:raise ValueError('short ELF header')
    fields=header.unpack_from(data,16);offset,size,count=fields[4],fields[8],fields[9]
    segment=struct.Struct(endian+('IIIIIIII' if data[4]==1 else 'IIQQQQQQ'))
    if fields[0]!=2 or not count or size!=segment.size or offset+size*count>len(data):raise ValueError('invalid ELF program table')
    virtual_ranges=[];physical_ranges=[]
    for index in range(count):
        values=segment.unpack_from(data,offset+index*size)
        if values[0]!=1:continue
        if data[4]==1:_,fileoff,virtual,physical,filesize,memsize,flags,align=values
        else:_,flags,fileoff,virtual,physical,filesize,memsize,align=values
        if not memsize or memsize<filesize or fileoff+filesize>len(data) or physical+memsize>0xffffffffffffffff or virtual+memsize>0xffffffffffffffff:raise ValueError('invalid ELF load extent')
        virtual_ranges.append((virtual,virtual+memsize));physical_ranges.append((physical,physical+memsize))
    if not virtual_ranges:raise ValueError('ELF has no load segments')
    def extent(ranges):
        first=min(a for a,b in ranges);last=max(b for a,b in ranges)
        return first,last-first
    return {'virtual':extent(virtual_ranges),'physical':extent(physical_ranges),'identity':virtual_ranges==physical_ranges}

def union(ranges):
    result=[]
    for first,last in sorted(ranges):
        if first>=last:raise ValueError('empty or wrapping interval')
        if result and first<=result[-1][1]:result[-1]=(result[-1][0],max(last,result[-1][1]))
        else:result.append((first,last))
    return result

def subtract(ranges,cuts):
    result=[]
    for first,last in ranges:
        cursor=first
        for begin,end in cuts:
            if end<=cursor or begin>=last:continue
            if begin>cursor:result.append((cursor,begin))
            cursor=max(cursor,min(last,end))
        if cursor<last:result.append((cursor,last))
    return result

def acceptance(arch,data,header_only=False,high=False,legacy_arm32=False):
    if b'FIRMWARE_DONE scope=actual-qemu-boot\n' not in data:raise ValueError('incomplete completion marker')
    facts=firmware.acceptance(arch,data,header_only,high,legacy_arm32=legacy_arm32)
    lines=data.decode(errors='replace').splitlines()
    memory=[line for line in lines if line.startswith('MEMORY_')]
    frame_lines=[line for line in lines if line.startswith('FRAME_NATIVE')]
    if len(frame_lines)!=1:raise ValueError('missing or duplicate native frame result')
    def parse(prefix,names,expected=None):
        pattern=prefix+''.join(' '+name+r'=(0x[0-9a-f]{16})' for name in names)
        found=[]
        for line in memory:
            if line.startswith(prefix+' '):
                match=re.fullmatch(pattern,line)
                if not match:raise ValueError('malformed '+prefix)
                found.append(tuple(int(value,16) for value in match.groups()))
        if expected is not None and len(found)!=expected:raise ValueError('wrong count '+prefix)
        return found
    image=parse('MEMORY_IMAGE',['base','size'],1)[0]
    results=[re.fullmatch(r'MEMORY_RESULT status=(0x[0-9a-f]{16}) pages=(0x[0-9a-f]{16}) touched=(0x[0-9a-f]{16}) process_allocator=unchanged',line) for line in memory if line.startswith('MEMORY_RESULT')]
    if len(results)!=1 or not results[0]:raise ValueError('missing memory result')
    status,pages,touched=(int(value,16) for value in results[0].groups())
    ranks={'MEMORY_IMAGE':0,'MEMORY_POLICY':1,'MEMORY_LOADED':2,'MEMORY_MANAGED':3,'MEMORY_EXCLUDED':4,'MEMORY_RESULT':5}
    order=[ranks.get(line.split()[0],-1) for line in memory]
    first_at=lines.index(memory[0]);last_at=lines.index(memory[-1])
    firmware_result=next(i for i,line in enumerate(lines) if line.startswith('FIRMWARE_RESULT '))
    done=lines.index('FIRMWARE_DONE scope=actual-qemu-boot')
    if order!=sorted(order) or order[0]!=0 or order[-1]!=5 or not firmware_result<first_at<=last_at<done:raise ValueError('memory evidence ordering')
    if not first_at<lines.index(frame_lines[0])<last_at:raise ValueError('native frame evidence ordering')
    if facts['reason']!='none':
        if not status or pages or touched or len(memory)!=2:raise ValueError('unavailable firmware produced qualified RAM')
        native=re.findall(rb'FRAME_NATIVE status=(0x[0-9a-f]{16}) executed=(0x[0-9a-f]{16}) free=(0x[0-9a-f]{16}) total=(0x[0-9a-f]{16})\n',data)
        if len(native)!=1 or int(native[0][0],16)==0 or any(int(v,16) for v in native[0][1:]):raise ValueError('unsupported source allocated native frames')
        facts['memory']={'qualified':False,'pages':0,'touched':0}
        return facts
    policy=parse('MEMORY_POLICY',['base','size','ceiling'],1)[0]
    loaded=parse('MEMORY_LOADED',['base','size','ram'])
    managed=parse('MEMORY_MANAGED',['base','size'])
    excluded=parse('MEMORY_EXCLUDED',['base','size','reason'])
    if len(memory)!=3+len(loaded)+len(managed)+len(excluded):raise ValueError('unknown memory evidence')
    if status or pages!=1024 or touched!=pages:raise ValueError('qualification/page touch failed')
    if len(loaded)>64 or len(managed)>512 or len(excluded)>512:raise ValueError('range capacities exceeded')
    all_ranges=[image]+[(a,n) for a,n,r in loaded]+managed+[(a,n) for a,n,r in excluded]
    if any(not n or a+n>0xffffffffffffffff for a,n in all_ranges):raise ValueError('range wraps or is empty')
    if sum(n for a,n in managed)!=pages*4096:raise ValueError('managed byte count mismatch')
    if arch in ('i386','x86_64'):
        expected_policy=(0,0x40000000,0x100000000)
        expected_loaded=[(0,0x100000,0),(image[0],max(image[0]+image[1],0x500000)-image[0],1)]
        raw=union([(a,a+n) for a,n,t,f in facts['e820_records'] if n and t==1 and f&1])
        reserved=[(a,a+n) for a,n,t,f in facts['e820_records'] if n and (t!=1 or not f&1)]
    else:
        expected_policy=(0, min(int(facts['raw'][2],16),0x10000000),0x100000000) if arch=='ppc' else (0x40000000 if arch in ('aarch64','arm32') else 0x80000000,0x10000000 if arch=='arm32' else 0x20000000,0x100000000)
        expected_loaded=([(0,0x1000000,0)] if arch=='ppc' else [(0x40000000,0x80000,0)] if arch=='aarch64' else [(0x40000000,0x10000,0)] if arch=='arm32' else [])+[(image[0],image[1],1)]
        raw=union([(a,a+n) for a,n in facts['fdt_ram']])
        reserved=[(a,a+n) for a,n,*rest in facts['fdt_reservations']]
    if policy!=expected_policy or loaded!=expected_loaded:raise ValueError('entry policy/image reservation mismatch')
    reserved += [(a,a+n) for a,n,r in loaded]
    a,n=facts['source_storage'];reserved.append((a,a+n));reserved=union(reserved)
    expected=[]
    for a,b in raw:
        for page in range((max(a,policy[0])+4095)&~4095,min(b,policy[0]+policy[1],policy[2])&~4095,4096):
            if not any(page<last and first<page+4096 for first,last in reserved):expected.append(page)
            if len(expected)==1024:break
        if len(expected)==1024:break
    observed=[]
    for a,n in managed:
        if a%4096 or n%4096:raise ValueError('unaligned managed memory')
        observed.extend(range(a,a+n,4096))
    if observed!=expected:raise ValueError('managed pages differ from independent firmware subtraction')
    partition=[(a,a+n) for a,n in managed]+[(a,a+n) for a,n,r in excluded]
    if any(r>4 for a,n,r in excluded) or union(partition)!=raw:raise ValueError('source/exclusion conservation failed')
    ordered=sorted(partition)
    if any(ordered[i-1][1]>ordered[i][0] for i in range(1,len(ordered))):raise ValueError('overlapping output partition')
    remaining=subtract(raw,reserved)
    by_reason={0:subtract(raw,remaining)}
    accessible=[(policy[0],policy[0]+policy[1])]
    by_reason[1]=subtract(remaining,accessible);remaining=subtract(remaining,by_reason[1])
    by_reason[2]=subtract(remaining,[(0,policy[2])]);remaining=subtract(remaining,by_reason[2])
    aligned=[((a+4095)&~4095,b&~4095) for a,b in remaining if (a+4095)&~4095 < b&~4095]
    by_reason[3]=subtract(remaining,aligned)
    by_reason[4]=subtract(aligned,[(a,a+n) for a,n in managed])
    for reason in range(5):
        if union([(a,a+n) for a,n,r in excluded if r==reason])!=union(by_reason[reason]):raise ValueError('incorrect exclusion reason '+str(reason))
    native=re.findall(rb'FRAME_NATIVE status=(0x[0-9a-f]{16}) executed=(0x[0-9a-f]{16}) free=(0x[0-9a-f]{16}) total=(0x[0-9a-f]{16})\n',data)
    if len(native)!=1 or tuple(int(v,16) for v in native[0])!=(0,1,1024,1024):raise ValueError('native frame ownership/user store failed')
    facts['frame_allocator']={'native_store':True,'exhaustion':True,'retired_before_release':True,'reclaimed':1024,'process_runtime_installed':False}
    facts['memory']={'qualified':True,'pages':pages,'touched':touched,'policy':policy,'image':image,'managed':managed,'excluded':excluded}
    return facts

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel-root',type=Path,default=stage);p.add_argument('--output',type=Path,required=True);p.add_argument('--arch',choices=list(profiles),action='append');p.add_argument('--header-only',action='store_true');p.add_argument('--x86-high',action='store_true');p.add_argument('--aarch64-el2',action='store_true');a=p.parse_args()
    root=a.kernel_root.resolve();output=a.output.resolve();output.mkdir(parents=True,exist_ok=False);rows=[]
    selected=a.arch or list(profiles)
    if a.x86_high and any(arch not in ('i386','x86_64') for arch in selected):p.error('--x86-high requires x86-only selection')
    for arch in selected:
        out=output/arch;out.mkdir();snap=out/'sources';hashes={};cfg=profiles[arch]
        def freeze(path,name):
            dest=snap/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(path.read_bytes());hashes[name]=digest(dest);return dest
        freeze(Path(__file__),'run_native.py');freeze(stage/'scripts/test_native_firmware_handoff.py','test_native_firmware_handoff.py');freeze(stage/'scripts/native_firmware_profiles.json','build-profiles.json')
        for name in ['include/ok/arch/types.hpp','include/ok/core/types.hpp','include/ok/core/fixed.hpp','include/ok/boot/e820.hpp','include/ok/boot/fdt.hpp','include/ok/boot/raw_handoff.hpp','src/boot/raw_handoff.cpp','src/boot/fdt.cpp',f'src/arch/{arch}/boot.S',f'src/arch/{arch}/linker.ld']:
            freeze(root/name,name)
        freeze(stage/'include/ok/boot/firmware_handoff.hpp','include/ok/boot/firmware_handoff.hpp');freeze(stage/'tests/native/runtime_frame_probe.cpp','probe.cpp');freeze(stage/'include/ok/boot/qualified_memory.hpp','include/ok/boot/qualified_memory.hpp');freeze(stage/'include/ok/boot/qemu_entry_memory.hpp','include/ok/boot/qemu_entry_memory.hpp')
        for header in (root/'include').rglob('*.hpp'):
            relative=str(header.relative_to(root))
            if relative not in hashes:freeze(header,relative)
        if arch in ('mips','mips64'):
            for dependency in (root/'src/arch/mips_common').iterdir():
                if dependency.is_file():freeze(dependency,str(dependency.relative_to(root)))
        freeze(stage/'include/ok/boot/qemu_runtime_frames.hpp','include/ok/boot/qemu_runtime_frames.hpp')
        freeze(stage/'tests/native/runtime_frame_user.S','user.S')
        for relative in ['src/memory/memory.cpp',f'src/arch/{arch}/execution.cpp',f'src/arch/{arch}/execution.S']:
            freeze(root/relative,relative)
        prefix=root/'toolchains'/cfg['triple']/'bin'/cfg['triple'];tools={n:Path(str(prefix)+'-'+n) for n in ['gcc','g++','ld','objcopy']}
        if subprocess.check_output([str(tools['gcc']),'-dumpmachine'],text=True).strip()!=cfg['triple'] or subprocess.check_output([str(tools['gcc']),'-dumpfullversion'],text=True).strip()!=cfg['compiler']['version']:raise ValueError('compiler identity mismatch')
        window=windows.get(arch,(0,0));defines=cfg['defines']+[f'OK_PROBE_ARCH={arch}',f'OK_PROBE_PROTOCOL={protocols[arch]}',f'OK_PROBE_WINDOW_START={window[0]}ULL',f'OK_PROBE_WINDOW_END={window[1]}ULL']
        if a.header_only:defines+=['OK_PROBE_HEADER_ONLY']
        commands=[]
        def run(cmd,log):
            cmd=list(map(str,cmd));commands.append(cmd);subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT)
        with (out/'build.log').open('w') as log:
            run([tools['gcc'],*cfg['asflags'],'-c',snap/f'src/arch/{arch}/boot.S','-o',out/'boot.o'],log)
            objects=[out/'boot.o']
            for name,path in [('probe','probe.cpp'),('capture','src/boot/raw_handoff.cpp'),('fdt','src/boot/fdt.cpp'),('memory','src/memory/memory.cpp'),('execution',f'src/arch/{arch}/execution.cpp')]:
                obj=out/(name+'.o');run([tools['g++'],'-std=c++23',*cfg['cxxflags'],'-fno-builtin','-Werror','-fstack-usage','-ffunction-sections','-fdata-sections','-I'+str(snap/'include'),*['-D'+d for d in defines],'-c',snap/path,'-o',obj],log);objects.append(obj)
            for name,path in [('native-entry',f'src/arch/{arch}/execution.S'),('user','user.S')]:
                obj=out/(name+'.o');run([tools['gcc'],*cfg['asflags'],*['-D'+d for d in defines],'-c',snap/path,'-o',obj],log);objects.append(obj)
            run([tools['g++'],*cfg['ldflags'],'-Wl,--gc-sections','-nostdlib','-static','-Wl,--build-id=none','-Wl,-T,'+str(snap/f'src/arch/{arch}/linker.ld'),*objects,'-lgcc','-o',out/'probe.elf'],log)
            if cfg['image_format']=='elf':data=(out/'probe.elf').read_bytes()
            else:
                run([tools['objcopy'],'-O','binary',out/'probe.elf',out/'payload.bin'],log);data=(out/'payload.bin').read_bytes()
                if arch in ('i386','x86_64'):
                    boot=freeze(root/'src/arch/x86_64/boot16.S','src/arch/x86_64/boot16.S');run([tools['gcc'],'-m32','-c',boot,'-o',out/'boot16.o'],log);run([tools['ld'],'-m','elf_i386','-Ttext','0x7c00','--oformat','binary',out/'boot16.o','-o',out/'boot16.bin'],log)
                    sector=(out/'boot16.bin').read_bytes()
                    if len(sector)!=512 or sector[-2:]!=b'\x55\xaa' or len(data)>8*1024*1024:raise ValueError('invalid BIOS image size')
                    data=sector+data+bytes(8*1024*1024-len(data))
            (out/'kernel.bin').write_bytes(data)
        qemu=Path('/usr/bin/qemu-system-'+{'rv64':'riscv64','rv32':'riscv32','arm32':'arm'}.get(arch,arch));command=qemu_command(qemu,arch,out/'kernel.bin')
        if a.x86_high:command+=['-m','5G']
        if a.aarch64_el2 and arch=='aarch64':command[command.index('-M')+1]='virt,virtualization=on'
        child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);data=bytearray();deadline=time.monotonic()+20
        try:
            while time.monotonic()<deadline and b'FIRMWARE_DONE scope=actual-qemu-boot\n' not in data:
                if select.select([child.stdout],[],[],.1)[0]:
                    chunk=os.read(child.stdout.fileno(),8192)
                    if not chunk:break
                    data+=chunk
        finally:
            child.terminate()
            try:data+=child.communicate(timeout=2)[0]
            except subprocess.TimeoutExpired:child.kill();data+=child.communicate()[0]
        (out/'serial.log').write_bytes(data)
        row={'arch':arch,'status':'pass','scope':'actual-QEMU qualified frames, native userspace store, retirement and reclamation; process allocator unchanged','commands':commands,'qemu':command,'source_sha256':hashes,'tools_sha256':{str(path):digest(path) for path in tools.values()},'qemu_sha256':digest(qemu),'qemu_version':subprocess.check_output([str(qemu),'--version'],text=True).splitlines()[0],'artifact_sha256':{name:digest(out/name) for name in ['probe.elf','kernel.bin','serial.log','build.log']}}
        try:
            row['facts']=acceptance(arch,data,a.header_only,a.x86_high)
            row['elf_load_extents']=loaded_elf_extents((out/'probe.elf').read_bytes())
            match=re.search(rb'MEMORY_IMAGE base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16})\n',data)
            if not match or tuple(int(value,16) for value in match.groups())!=row['elf_load_extents']['virtual']:raise ValueError('linker image omits ELF loaded/NOLOAD extent')
            if arch in supported and not row['elf_load_extents']['identity']:raise ValueError('qualified entry image is not identity-linked')
        except ValueError as error:row.update(status='fail',failure=str(error))
        (out/'result.json').write_text(json.dumps(row,indent=2)+'\n');rows.append(row);(output/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(arch,row['status'],row.get('facts',row.get('failure')),flush=True)
    return int(any(row['status']!='pass' for row in rows))
if __name__=='__main__':raise SystemExit(main())
