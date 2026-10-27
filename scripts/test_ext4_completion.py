#!/usr/bin/env python3
"""Qualify bounded recovery completion and interrupted-write states on private files."""
from __future__ import annotations
import argparse,hashlib,json,os,re,shlex,shutil,struct,subprocess,sys,time
from pathlib import Path
PROFILES = (
    ("i386", "i386-elf", 1, 1, 3), ("x86_64", "x86_64-elf", 2, 1, 62),
    ("aarch64", "aarch64-elf", 2, 1, 183), ("arm32", "arm-none-eabi", 1, 1, 40),
    ("rv64", "riscv64-elf", 2, 1, 243), ("rv32", "riscv32-elf", 1, 1, 243),
    ("loongarch64", "loongarch64-elf", 2, 1, 258), ("mips", "mips-elf", 1, 2, 8),
    ("mips64", "mips64-elf", 2, 2, 8), ("ppc", "powerpc-eabi", 1, 2, 20),
)
TARGET_FLAGS = {
    "i386": ["-mno-sse", "-mno-sse2"], "x86_64": ["-mno-red-zone"],
    "aarch64": ["-mno-outline-atomics", "-mgeneral-regs-only", "-mstrict-align"], "arm32": ["-march=armv7-a", "-marm", "-mno-unaligned-access"],
    "rv64": ["-mcmodel=medany"], "rv32": ["-mcmodel=medany"], "loongarch64": [],
    "mips": ["-march=mips32r2", "-G0"], "mips64": ["-march=mips64r2", "-mabi=64", "-G0"],
    "ppc": ["-msoft-float", "-msdata=none"],
}


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    source=Path(__file__).resolve().parents[1]
    parser.add_argument('--kernel-root',type=Path,default=source)
    parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cxx',default=os.environ.get('CXX','g++'))
    parser.add_argument('--toolchain-root',type=Path)
    parser.add_argument('--sanitize',action='store_true')
    parser.add_argument('--cross',action='store_true')
    args=parser.parse_args();root=args.kernel_root.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    report={'scope':'hosted-bounded-ext4-recovery-completion','status':'running','commands':[],
            'sanitize':args.sanitize,'asan_options':os.environ.get('ASAN_OPTIONS',''),'native_execution':False,
            'physical_power_loss':False,'torn_control_reconstruction':False,'mount_admission':False,'journal_writer':False}
    def save(): (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(command,label,allowed=(0,),cwd=None):
        record={'argv':[str(x) for x in command],'label':label,'status':'running','cwd':str(cwd) if cwd else None};report['commands'].append(record);save()
        start=time.monotonic();result=subprocess.run(record['argv'],text=True,capture_output=True,cwd=cwd)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        record.update(status='passed' if result.returncode in allowed else 'failed',returncode=result.returncode,seconds=time.monotonic()-start);save()
        if result.returncode not in allowed:raise RuntimeError(label+' failed; inspect retained output')
        return result.stdout+result.stderr
    save()
    try:
        fixture=args.fixture.resolve();capture=json.loads((fixture/'capture-manifest.json').read_text())
        disk=out/'linux-dirty-capture.img';shutil.copyfile(fixture/'linux-dirty-capture.img',disk)
        if digest(disk)!=capture['capture_sha256']:raise ValueError('Linux fixture identity mismatch')
        (out/'capture-manifest.json').write_bytes((fixture/'capture-manifest.json').read_bytes())
        report.update(capture_sha256=digest(disk),linux_image_sha256=capture['linux_image_sha256'],linux_source_provenance=capture['linux_source_provenance'])
        frozen=out/'sources';shutil.copytree(root/'include',frozen/'include')
        paths=['include/ok/fs/ext4/txn.hpp','include/ok/fs/ext4/layout.hpp',
               'tests/host/ext4_completion_tests.cpp','tests/host/ext4_completion_support.hpp',
               'tests/host/ext4_completion_compile.cpp','scripts/test_ext4_completion.py']
        for relative in paths:
            target=frozen/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((source/relative).read_bytes())
        impl=frozen/'src/fs/ext4_recovery.cpp';impl.parent.mkdir(parents=True,exist_ok=True);impl.write_bytes((root/'src/fs/ext4_recovery.cpp').read_bytes())
        report['source_sha256']={str(p.relative_to(frozen)):digest(p) for p in sorted(frozen.rglob('*')) if p.is_file()}
        flags=['-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti']
        binary=out/'completion-tests'
        run([*shlex.split(args.cxx),*flags,'-DOK_ARCH_TARGET_X86_64','-I'+str(frozen/'include'),
             frozen/'tests/host/ext4_completion_tests.cpp',impl,'-Wl,-l:libext2fs.so.2',
             *(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []),'-o',binary],'compile-hosted')
        text=run([binary,disk,out/'model'],'hosted');print(text,end='',flush=True)
        report['model_counts']={key:int(value) for key,value in re.findall(r'(cases|restarts|torn_rejections|write_byte_boundaries|reads)=(\d+)',text)}
        if digest(disk)!=capture['capture_sha256']:raise ValueError('source capture modified')
        run(['e2fsck','-fn',out/'model/completed.img'],'completed-readonly-e2fsck')
        run(['debugfs','-R','stat /seed',out/'model/completed.img'],'completed-seed-reference')
        budget=out/'budget.img';shutil.copyfile(out/'model/completed.img',budget)
        (out/'budget-empty.bin').write_bytes(b'');script=out/'budget.debugfs'
        script.write_text(''.join('write budget-empty.bin /many-'+str(i)+'\n' for i in range(129)))
        run(['debugfs','-w','-f',script,budget],'prepare-inode-budget',cwd=out)
        run(['e2fsck','-fn',budget],'inode-budget-readonly-e2fsck')
        print(run([binary,budget,out/'budget-model','budget'],'hosted-budget'),end='',flush=True)
        report['budget_cases']=1
        control=out/'e2fsprogs-recovery.img';shutil.copyfile(disk,control)
        run(['e2fsck','-fy','-E','journal_only',control],'independent-journal-recovery',(0,1))
        run(['e2fsck','-fn',control],'independent-readonly-e2fsck')
        actual=(out/'model/completed.img').read_bytes();reference=control.read_bytes()
        differences=[i for i in range(len(actual)) if actual[i]!=reference[i]]
        bookkeeping=set(range(1024+48,1024+54))|set(range(1024+64,1024+68))|set(range(1024+376,1024+384))|set(range(2044,2048))
        if any(i not in bookkeeping for i in differences):raise ValueError('reference differs outside explicit e2fsck bookkeeping')
        report['reference_bookkeeping_byte_differences']=differences
        report['e2fsprogs_version']=run(['e2fsck','-V'],'e2fsprogs-version').strip()
        if args.cross:
            toolchains=args.toolchain_root.resolve() if args.toolchain_root else root/'toolchains'
            for arch,triple,elf_class,endian,machine in PROFILES:
                compiler=toolchains/triple/'bin'/(triple+'-g++');obj=out/(arch+'.o')
                run([compiler,*flags,'-ffreestanding','-fno-threadsafe-statics','-fno-use-cxa-atexit',*TARGET_FLAGS[arch],
                     '-DOK_ARCH_TARGET_'+arch.upper(),'-I'+str(frozen/'include'),'-c',frozen/'tests/host/ext4_completion_compile.cpp','-o',obj],'cross-'+arch)
                data=obj.read_bytes();order='<' if endian==1 else '>'
                if data[:4]!=b'\x7fELF' or data[4:6]!=bytes((elf_class,endian)) or struct.unpack_from(order+'H',data,18)[0]!=machine:
                    raise ValueError('cross-object identity mismatch: '+arch)
                print('PASS freestanding ext4 completion compile '+arch+' (not executed)',flush=True)
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));print('FAIL '+str(error),file=sys.stderr)
    finally:
        report['artifact_sha256']={str(p.relative_to(out)):digest(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name!='result.json'};save()
    return 0 if report['status']=='passed' else 1

if __name__=='__main__':raise SystemExit(main())
