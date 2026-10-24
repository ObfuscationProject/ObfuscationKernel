#!/usr/bin/env python3
"""Isolated actual-QEMU typed firmware discovery; never enables an allocator."""
from pathlib import Path
import argparse, hashlib, json, os, re, select, subprocess, sys, time
sys.dont_write_bytecode=True
stage=Path(__file__).resolve().parents[1]
profiles=json.loads((stage/'scripts/native_firmware_profiles.json').read_text())
protocols={'i386':'bios_e820','x86_64':'bios_e820','aarch64':'aarch64_linux_image','arm32':'arm32_linux_image','rv64':'riscv_fdt','rv32':'riscv_fdt','loongarch64':'loongarch64_direct_elf','mips':'malta_prom','mips64':'malta_prom','ppc':'epapr'}
windows={'arm32':(0x40000000,0x60000000),'i386':(0x5000,0x5c10),'x86_64':(0x5000,0x5c10),'aarch64':(0x40000000,0x60000000),'rv64':(0x80000000,0xa0000000),'rv32':(0x80000000,0xa0000000),'ppc':(0,0x10000000)}
supported=set(windows)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def acceptance(arch,data,header_only=False,high=False,arm32_direct_elf=False,legacy_arm32=False):
    text=data.decode(errors='replace');lines=text.splitlines()
    def unique(pattern):
        found=[(i,re.fullmatch(pattern,line)) for i,line in enumerate(lines) if re.fullmatch(pattern,line)]
        if len(found)!=1:raise ValueError('expected unique whole line: '+pattern)
        return found[0]
    raw_at,raw=unique(r'FIRMWARE_RAW captured=0x0000000000000001 values=((?:0x[0-9a-f]{16} ){4})')
    result_at,result=unique(r'FIRMWARE_RESULT reason=([a-z_]+) status=(0x[0-9a-f]{16}) reads=(0x[0-9a-f]{16}) allocator=unqualified')
    done_at,_=unique(r'FIRMWARE_DONE scope=actual-qemu-boot')
    if not raw_at<result_at<done_at or any('FAIL' in line for line in lines):raise ValueError('guest failure or ordering')
    for name in ('FIRMWARE_RAW','FIRMWARE_RESULT','FIRMWARE_DONE'):
        if text.count(name)!=1:raise ValueError('duplicate or malformed '+name)
    seen={raw_at,result_at,done_at}
    words=[int(v,16) for v in raw[1].split()]
    reads=[]
    for i,line in enumerate(lines):
        if line.startswith('FIRMWARE_READ'):
            m=re.fullmatch(r'FIRMWARE_READ address=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16})',line)
            if not m or not raw_at<i<result_at:raise ValueError('invalid reader evidence')
            reads.append(tuple(int(v,16) for v in m.groups()));seen.add(i)
    if len(reads)!=int(result[3],16):raise ValueError('reader count mismatch')
    expected=('protocol_mismatch' if arm32_direct_elf else 'source_absent') if arch=='arm32' and (arm32_direct_elf or legacy_arm32) else ('unmapped_source' if header_only else 'none') if arch in supported else ('source_absent' if arch in ('arm32','loongarch64') else 'unsupported_protocol')
    if result[1]!=expected or (int(result[2],16)==0)!=(expected=='none'):raise ValueError('unexpected discovery result: '+result[0])
    facts={'raw':[hex(v) for v in words],'reason':expected,'reads':reads,'allocator_qualified':False}
    if arch=='arm32' and not legacy_arm32:
        entry_at,entry=unique(r'ARM32_ENTRY cpsr=(0x[0-9a-f]{16}) sctlr=(0x[0-9a-f]{16}) mpidr=(0x[0-9a-f]{16}) valid=0x0000000000000001')
        cpsr,sctlr,mpidr=(int(v,16) for v in entry.groups())
        if entry_at>=raw_at or cpsr&0x23f!=0x13 or sctlr&0x02000005 or mpidr&0xffffff:raise ValueError('ARM entry state rejected')
        facts['entry_state']={'cpsr':cpsr,'sctlr':sctlr,'mpidr':mpidr}
        if text.count('ARM32_ENTRY')!=1:raise ValueError('duplicate ARM entry state')
    if expected!='none':
        if reads and not (header_only and len(reads)==2):raise ValueError('unexpected read for rejected protocol')
        if any(line.startswith(('FIRMWARE_STORAGE','E820_','FDT_')) for line in lines):raise ValueError('failed discovery published map')
        if (arch=='loongarch64' or (arch=='arm32' and (arm32_direct_elf or legacy_arm32))) and any(words):raise ValueError('unexpected direct ELF arguments')
        if any(i not in seen and line.startswith(('FIRMWARE_','E820_','FDT_')) for i,line in enumerate(lines)):raise ValueError('unknown or malformed firmware diagnostics')
        return facts
    storage_at,storage=unique(r'FIRMWARE_STORAGE base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16}) decoded=(0x[0-9a-f]{16})')
    seen.add(storage_at)
    address,size,decoded=(int(v,16) for v in storage.groups())
    source_word=2 if arch=='arm32' else 1 if arch in ('i386','x86_64','rv32','rv64') else 0
    if address!=words[source_word]:raise ValueError('source address differs from captured argument')
    if arch=='arm32' and (words[0] or words[1]!=0xffffffff or not words[2] or words[2]&7 or words[2]>0xffffffff):raise ValueError('ARM Linux header identity mismatch')
    if arch=='aarch64' and any(words[1:]):raise ValueError('nonzero Linux Image reserved words')
    if arch=='ppc' and (words[1]!=0x45504150 or words[3]):raise ValueError('ePAPR identity mismatch')
    start,end=windows[arch]
    if len(reads)!=2 or reads[0]!=(address,16 if arch in ('i386','x86_64') else 40) or reads[1]!=(address,decoded) or not start<=address<address+size<=end:raise ValueError('bounded mapping evidence mismatch')
    if arch=='ppc' and address+size>words[2]:raise ValueError('FDT exceeds ePAPR initial mapping')
    if not raw_at<storage_at<result_at or any(i>storage_at and line.startswith('FIRMWARE_READ') for i,line in enumerate(lines)):raise ValueError('map ordering')
    if arch in ('i386','x86_64'):
        if words!=[0x30323845,0x5000,0,0]:raise ValueError('wrong BIOS identity')
        map_at,count=unique(r'E820_MAP count=(0x[0-9a-f]{16})')
        seen.add(map_at)
        records=[]
        for i,line in enumerate(lines):
            if line.startswith('E820_RECORD'):
                m=re.fullmatch(r'E820_RECORD base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16}) type=(0x[0-9a-f]{16}) attributes=(0x[0-9a-f]{16})',line)
                if not m or not map_at<i<result_at:raise ValueError('bad E820 record')
                records.append(tuple(int(v,16) for v in m.groups()));seen.add(i)
        if not storage_at<map_at or len(records)!=int(count[1],16) or decoded!=16+len(records)*24 or size!=16+128*24 or not 1<=len(records)<=128:raise ValueError('E820 count/size mismatch')
        if any(a+n>0xffffffffffffffff or t>0xffffffff or flags>0xffffffff for a,n,t,flags in records):raise ValueError('truncated/wrapping E820 record')
        if not any(n and t==1 and flags&1 for a,n,t,flags in records):raise ValueError('E820 usable RAM absent')
        if high and not any(a>=0x100000000 and n and t==1 and flags&1 for a,n,t,flags in records):raise ValueError('full-u64 high RAM absent')
        facts['e820_records']=records
    else:
        map_at,count=unique(r'FDT_MAP ram=(0x[0-9a-f]{16}) reservations=(0x[0-9a-f]{16}) boot_cpu=(0x[0-9a-f]{16})')
        seen.add(map_at)
        ram=[];reserved=[]
        for i,line in enumerate(lines):
            if line.startswith('FDT_RAM'):
                m=re.fullmatch(r'FDT_RAM base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16})',line)
                if not m or not map_at<i<result_at:raise ValueError('bad FDT RAM')
                ram.append(tuple(int(v,16) for v in m.groups()));seen.add(i)
            if line.startswith('FDT_RESERVED'):
                m=re.fullmatch(r'FDT_RESERVED base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16}) source=(0x[0-9a-f]{16}) no_map=(0x[0-9a-f]{16}) reusable=(0x[0-9a-f]{16})',line)
                if not m or not map_at<i<result_at:raise ValueError('bad FDT reservation')
                reserved.append(tuple(int(v,16) for v in m.groups()));seen.add(i)
        if not storage_at<map_at or len(ram)!=int(count[1],16) or len(reserved)!=int(count[2],16):raise ValueError('FDT count mismatch')
        if decoded!=size:raise ValueError('FDT storage and decoded sizes differ')
        if ram!=[(start,end-start)]:raise ValueError('FDT RAM differs from explicit QEMU -m contract: '+str(ram))
        if any(not n or a+n>0xffffffffffffffff for a,n in ram) or any(not n or a+n>0xffffffffffffffff or source>1 or no_map>1 or reusable>1 or(no_map and reusable) for a,n,source,no_map,reusable in reserved):raise ValueError('invalid FDT range or reservation')
        facts.update(fdt_ram=ram,fdt_reservations=reserved)
    if any(i not in seen and line.startswith(('FIRMWARE_','E820_','FDT_')) for i,line in enumerate(lines)):raise ValueError('unknown or malformed firmware diagnostics')
    facts['source_storage']=(address,size)
    facts['decoded_bytes']=decoded
    return facts

def qemu_command(qemu,arch,image):
    common=['-serial','stdio','-monitor','none','-no-reboot','-display','none']
    if arch in ('i386','x86_64'):
        cpu=['-cpu','max'] if arch=='i386' else []
        return [str(qemu),'-m','128M','-drive',f'file={image},format=raw,if=ide','-boot','c','-vga','none',*cpu,*common]
    if arch in ('aarch64','arm32'):
        return [str(qemu),'-M','virt','-cpu','cortex-a57' if arch=='aarch64' else 'cortex-a15','-m','512M','-kernel',str(image),*common]
    if arch in ('rv64','rv32'):
        return [str(qemu),'-M','virt','-m','512M','-bios','none','-kernel',str(image),*common]
    if arch in ('mips','mips64','ppc'):
        cpu=['-cpu','MIPS64R2-generic'] if arch=='mips64' else []
        return [str(qemu),'-M','ppce500' if arch=='ppc' else 'malta',*cpu,'-m','256M','-kernel',str(image),*common]
    return [str(qemu),'-M','virt','-m','2G','-kernel',str(image),*common]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel-root',type=Path,default=stage);p.add_argument('--output',type=Path,required=True);p.add_argument('--arch',choices=list(profiles),action='append');p.add_argument('--arm32-direct-elf',action='store_true');p.add_argument('--header-only',action='store_true');p.add_argument('--x86-high',action='store_true');p.add_argument('--aarch64-el2',action='store_true');a=p.parse_args()
    root=a.kernel_root.resolve();output=a.output.resolve();output.mkdir(parents=True,exist_ok=False);rows=[]
    selected=a.arch or list(profiles)
    if a.x86_high and any(arch not in ('i386','x86_64') for arch in selected):p.error('--x86-high requires x86-only selection')
    if a.arm32_direct_elf and (selected!=['arm32'] or a.header_only):p.error('--arm32-direct-elf requires only ARM32 and no shortened reader')
    for arch in selected:
        out=output/arch;out.mkdir();snap=out/'sources';hashes={};cfg=profiles[arch]
        def freeze(path,name):
            dest=snap/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(path.read_bytes());hashes[name]=digest(dest);return dest
        freeze(Path(__file__),'run_native.py');freeze(stage/'scripts/native_firmware_profiles.json','build-profiles.json')
        for name in ['include/ok/arch/types.hpp','include/ok/core/types.hpp','include/ok/boot/e820.hpp','include/ok/boot/fdt.hpp','include/ok/boot/raw_handoff.hpp','src/boot/raw_handoff.cpp','src/boot/fdt.cpp',f'src/arch/{arch}/boot.S',f'src/arch/{arch}/linker.ld']:
            freeze(root/name,name)
        freeze(stage/'include/ok/boot/firmware_handoff.hpp','include/ok/boot/firmware_handoff.hpp');freeze(stage/'tests/native/firmware_handoff_probe.cpp','probe.cpp')
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
            for name,path in [('probe','probe.cpp'),('capture','src/boot/raw_handoff.cpp'),('fdt','src/boot/fdt.cpp')]:
                obj=out/(name+'.o');run([tools['g++'],'-std=c++23',*cfg['cxxflags'],'-fno-builtin','-Werror','-fstack-usage','-I'+str(snap/'include'),*['-D'+d for d in defines],'-c',snap/path,'-o',obj],log);objects.append(obj)
            run([tools['g++'],*cfg['ldflags'],'-nostdlib','-static','-Wl,--build-id=none','-Wl,-T,'+str(snap/f'src/arch/{arch}/linker.ld'),*objects,'-lgcc','-o',out/'probe.elf'],log)
            if cfg['image_format']=='elf':data=(out/'probe.elf').read_bytes()
            else:
                run([tools['objcopy'],'-O','binary',out/'probe.elf',out/'payload.bin'],log);data=(out/'payload.bin').read_bytes()
                if arch in ('i386','x86_64'):
                    boot=freeze(root/'src/arch/x86_64/boot16.S','src/arch/x86_64/boot16.S');run([tools['gcc'],'-m32','-c',boot,'-o',out/'boot16.o'],log);run([tools['ld'],'-m','elf_i386','-Ttext','0x7c00','--oformat','binary',out/'boot16.o','-o',out/'boot16.bin'],log)
                    sector=(out/'boot16.bin').read_bytes()
                    if len(sector)!=512 or sector[-2:]!=b'\x55\xaa' or len(data)>8*1024*1024:raise ValueError('invalid BIOS image size')
                    data=sector+data+bytes(8*1024*1024-len(data))
            (out/'kernel.bin').write_bytes(data)
        qemu=Path('/usr/bin/qemu-system-'+{'rv64':'riscv64','rv32':'riscv32','arm32':'arm'}.get(arch,arch));command=qemu_command(qemu,arch,out/('probe.elf' if a.arm32_direct_elf else 'kernel.bin'))
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
        row={'arch':arch,'status':'pass','scope':'actual-QEMU captured arguments, bounded source mapping, typed discovery; allocator unqualified','commands':commands,'qemu':command,'source_sha256':hashes,'tools_sha256':{str(path):digest(path) for path in tools.values()},'qemu_sha256':digest(qemu),'qemu_version':subprocess.check_output([str(qemu),'--version'],text=True).splitlines()[0],'artifact_sha256':{name:digest(out/name) for name in ['probe.elf','kernel.bin','serial.log','build.log']}}
        try:row['facts']=acceptance(arch,data,a.header_only,a.x86_high,a.arm32_direct_elf)
        except ValueError as error:row.update(status='fail',failure=str(error))
        (out/'result.json').write_text(json.dumps(row,indent=2)+'\n');rows.append(row);(output/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(arch,row['status'],row.get('facts',row.get('failure')),flush=True)
    return int(any(row['status']!='pass' for row in rows))
if __name__=='__main__':raise SystemExit(main())
