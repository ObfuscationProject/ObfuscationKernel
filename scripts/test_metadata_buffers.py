#!/usr/bin/env python3
"""Qualify bounded synchronous block-session ownership; no native SMP claim."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,struct,subprocess,sys,time
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

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);source=Path(__file__).resolve().parents[1]
 p.add_argument('--kernel-root',type=Path,default=source);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--sanitize',action='store_true');p.add_argument('--cross',action='store_true')
 a=p.parse_args();root=a.kernel_root.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 r={'scope':'hosted-canonical-metadata-buffer-ownership','status':'running','commands':[],'sanitize':a.sanitize,
    'asan_options':os.environ.get('ASAN_OPTIONS',''),'native_smp':False,'physical_dma_fencing':False,'mount_admission':False}
 def save():(out/'result.json').write_text(json.dumps(r,indent=2)+'\n')
 def run(command,label):
  item={'argv':[str(x) for x in command],'label':label,'status':'running'};r['commands'].append(item);save();start=time.monotonic()
  result=subprocess.run(item['argv'],text=True,capture_output=True);(out/(label+'.log')).write_text(result.stdout+result.stderr)
  item.update(returncode=result.returncode,seconds=time.monotonic()-start,status='passed' if not result.returncode else 'failed');save()
  if result.returncode:raise RuntimeError(label+' failed')
  return result.stdout+result.stderr
 save()
 try:
  frozen=out/'sources';shutil.copytree(root/'include',frozen/'include')
  for relative in ['include/ok/block/sessions.hpp','include/ok/fs/metadata_buffers.hpp','tests/host/metadata_buffers_tests.cpp','tests/host/metadata_buffers_compile.cpp','tests/host/metadata_session_invalidation_tests.cpp','scripts/test_metadata_buffers.py']:
   target=frozen/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((source/relative).read_bytes())
  r['source_sha256']={str(f.relative_to(frozen)):sha(f) for f in sorted(frozen.rglob('*')) if f.is_file()}
  flags=['-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti']
  binary=out/'buffer-tests'
  run(['g++',*flags,'-DOK_ARCH_TARGET_X86_64','-I'+str(frozen/'include'),frozen/'tests/host/metadata_buffers_tests.cpp','-pthread',
       *(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else []),'-o',binary],'compile-hosted')
  text=run([binary],'hosted');print(text,end='',flush=True);r['checks']=int(re.search(r'checks=(\d+)',text).group(1));r['host_thread_concurrency']=True
  invalidation_binary=out/'session-invalidation-tests'
  run(['g++',*flags,'-DOK_ARCH_TARGET_X86_64','-I'+str(frozen/'include'),frozen/'tests/host/metadata_session_invalidation_tests.cpp','-pthread',
       *(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else []),'-o',invalidation_binary],'compile-session-invalidation')
  invalidation_text=run([invalidation_binary],'session-invalidation');print(invalidation_text,end='',flush=True)
  r['session_invalidation_checks']=int(re.search(r'checks=(\d+)',invalidation_text).group(1))
  if a.cross:
   for arch,triple,elf_class,endian,machine in PROFILES:
    obj=out/(arch+'.o');compiler=root/'toolchains'/triple/'bin'/(triple+'-g++')
    run([compiler,*flags,'-ffreestanding','-fno-threadsafe-statics','-fno-use-cxa-atexit',*TARGET_FLAGS[arch],'-DOK_ARCH_TARGET_'+arch.upper(),
         '-I'+str(frozen/'include'),'-c',frozen/'tests/host/metadata_buffers_compile.cpp','-o',obj],'cross-'+arch)
    data=obj.read_bytes();order='<' if endian==1 else '>'
    if data[:4]!=b'\x7fELF' or data[4:6]!=bytes((elf_class,endian)) or struct.unpack_from(order+'H',data,18)[0]!=machine:raise ValueError('cross-object identity')
    print('PASS freestanding metadata buffer compile '+arch+' (not executed)',flush=True)
  r['status']='passed'
 except Exception as error:r.update(status='failed',error=str(error));print('FAIL '+str(error),file=sys.stderr)
 finally:
  r['artifact_sha256']={str(f.relative_to(out)):sha(f) for f in sorted(out.rglob('*')) if f.is_file() and f.name!='result.json'};save()
 return 0 if r['status']=='passed' else 1
if __name__=='__main__':raise SystemExit(main())
