#!/usr/bin/env python3
"""Check the bounded first-block owner/mounted path on a pinned private fixture.

The input must have inode14 as the canonical SDK46-created /empty-note record,
with a dirty empty journal. This is hosted integration, never target execution.
"""
from pathlib import Path
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, time
HERE=Path(__file__).resolve().parent
K=HERE.parent
from test_ext4_mounted import SOURCES
from test_ext4_owned import PROFILES,TARGET_FLAGS
P=argparse.ArgumentParser()
P.add_argument('--sanitize',action='store_true')
P.add_argument('--cross',action='store_true')
P.add_argument('--toolchain-root',type=Path)
P.add_argument('--created-empty-image',type=Path,required=True)
P.add_argument('--input-sha256',required=True)
P.add_argument('--output',type=Path,required=True)
args=P.parse_args()
if not (__debug__ and sys.dont_write_bytecode and os.environ.get('PYTHONDONTWRITEBYTECODE') == '1'):
 raise ValueError('bytecode-disabled non-optimized Python required')
OUT=args.output.resolve();OUT.mkdir(exist_ok=False)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
CLEAN=args.created_empty_image.resolve()
report=dict(schema=1,scope='hosted private first-data owner and mounted POSIX path, fault restart and independent filesystem replay',status='running',driver_sha256=sha(Path(__file__)),source_sdk_abi=47,native_user_elf=False,qemu=False,physical=False,full_m0_m12=False,promoted=False,sanitize=args.sanitize,commands=[])
def save(): (OUT/'result-v1.json').write_text(json.dumps(report,indent=2)+'\n')
def run(argv,label,allowed=(0,)):
 row=dict(argv=[str(x) for x in argv],label=label,status='running');report['commands'].append(row);save()
 t=time.monotonic();r=subprocess.run(row['argv'],text=True,capture_output=True,timeout=300)
 log=OUT/(label+'.log');log.write_text(r.stdout+r.stderr)
 row.update(exit=r.returncode,status='passed' if r.returncode in allowed else 'failed',seconds=time.monotonic()-t,log_sha256=sha(log));save()
 if r.returncode not in allowed:raise RuntimeError(label+' failed')
 return r.stdout+r.stderr
try:
 shutil.copyfile(Path(__file__),OUT/'source-driver.py')
 if sha(CLEAN) != args.input_sha256: raise ValueError('pinned created-empty fixture changed')
 SN=OUT/'source';shutil.copytree(K/'include',SN/'include')
 names=SOURCES+['src/arch/ops_private.hpp','tests/host/ext4_realtime_model.hpp','tests/host/ext4_owned_first_data_tests.cpp','tests/host/ext4_mounted_first_data_tests.cpp','tests/host/ext4_owned_support.hpp','tests/host/ext4_transaction_support.hpp','tests/host/ext4_mounted_support.hpp','tests/host/ext4_first_data_compile.cpp']
 for name in names:
  target=SN/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(K/name,target)
 report['source_sha256']={str(p.relative_to(SN)):sha(p) for p in sorted(SN.rglob('*')) if p.is_file()}
 clean=OUT/'clean-input.img';shutil.copyfile(CLEAN,clean);report['original_clean_sha256']=sha(CLEAN)
 report['clean_sha256']=sha(clean)
 run(['e2fsck','-fn',clean],'input-fsck')
 flags=['-std=c++23','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DOK_ARCH_TARGET_X86_64','-I'+str(SN/'include')]
 if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 for name in ('ext4_owned_first_data_tests', 'ext4_mounted_first_data_tests'):
  binary=OUT/name
  run(['g++',*flags,SN/('tests/host/'+name+'.cpp'),*[SN/item for item in SOURCES],'-Wl,-l:libext2fs.so.2','-pthread','-o',binary],'compile-'+name)
  argument=OUT/'owner-model' if name=='ext4_owned_first_data_tests' else OUT/'mounted-created.img'
  print(run([binary,clean,argument],name),flush=True)
 run(['e2fsck','-fn',OUT/'owner-model/completed.img'],'owner-completed-fsck')
 run(['e2fsck','-fn',OUT/'mounted-created.img'],'mounted-completed-fsck')
 reference=OUT/'independent-replay.img';shutil.copyfile(OUT/'owner-model/published.img',reference)
 run(['e2fsck','-fy','-E','journal_only',reference],'independent-replay',(0,1))
 run(['e2fsck','-fn',reference],'independent-fsck')
 run(['debugfs','-R','stat /empty-note',reference],'independent-inode')
 actual=(OUT/'owner-model/completed.img').read_bytes();independent=reference.read_bytes()
 diffs=[i for i in range(len(actual)) if actual[i]!=independent[i]]
 allowed=set(range(1024+48,1024+54))|set(range(1024+64,1024+68))|set(range(1024+96,1024+100))|set(range(1024+376,1024+384))|set(range(2044,2048))
 if any(i not in allowed for i in diffs):raise ValueError('independent replay differs outside explicit recovery-bit/fsck bookkeeping')
 report['reference_bookkeeping_differences']=diffs
 if args.cross:
  toolchains=args.toolchain_root.resolve() if args.toolchain_root else K/'toolchains'
  for arch,triple,elf_class,endian,machine in PROFILES:
   compiler=toolchains/triple/'bin'/(triple+'-g++');obj=OUT/(arch+'.o')
   report.setdefault('compiler_sha256',{})[arch]=sha(compiler)
   run([compiler,'-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti','-ffreestanding','-fno-threadsafe-statics','-fno-use-cxa-atexit',*TARGET_FLAGS[arch],'-DOK_ARCH_TARGET_'+arch.upper(),'-I'+str(SN/'include'),'-c',SN/'tests/host/ext4_first_data_compile.cpp','-o',obj],'cross-'+arch)
   data=obj.read_bytes();order='<' if endian==1 else '>'
   if data[:4]!=b'\x7fELF' or data[4:6]!=bytes((elf_class,endian)) or struct.unpack_from(order+'H',data,18)[0]!=machine:raise ValueError('cross identity mismatch '+arch)
 report['input_unchanged']=sha(CLEAN)==report['original_clean_sha256'] and sha(clean)==report['clean_sha256']
 if not report['input_unchanged']:raise ValueError('original input modified')
 report['status']='passed'
except Exception as error:
 report.update(status='failed',error=str(error));print('FAIL '+str(error),file=sys.stderr)
finally:
 report['artifact_sha256']={str(p.relative_to(OUT)):sha(p) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='result-v1.json'};save()
sys.exit(report['status']!='passed')
