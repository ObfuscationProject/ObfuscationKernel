#!/usr/bin/env python3
"""Qualify escaped logical inode bytes and physical partition guards on host models."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--clean-image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 a=p.parse_args();root=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
 r={'scope':'hosted-owned-ext4-escaped-and-partition-edges','status':'running','sanitize':True,'native_execution':False,'commands':[], 'asan_options':os.environ.get('ASAN_OPTIONS','')}
 def save():(out/'result.json').write_text(json.dumps(r,indent=2)+'\n')
 def run(cmd,label):
  item={'argv':[str(x) for x in cmd],'label':label};r['commands'].append(item);save();result=subprocess.run(item['argv'],capture_output=True,text=True)
  (out/(label+'.log')).write_text(result.stdout+result.stderr);item['returncode']=result.returncode;save()
  if result.returncode:raise RuntimeError(label+' failed')
  print(result.stdout,end='',flush=True)
 try:
  src=out/'sources';shutil.copytree(root/'include',src/'include')
  for rel in ['scripts/test_ext4_owned_edges.py','tests/host/ext4_owned_edge_tests.cpp','tests/host/ext4_owned_support.hpp','tests/host/ext4_transaction_support.hpp','src/fs/ext4_recovery.cpp']:
   dest=src/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/rel,dest)
  r['source_sha256']={str(f.relative_to(src)):sha(f) for f in sorted(src.rglob('*')) if f.is_file()}
  disk=out/'clean-input.img';shutil.copyfile(a.clean_image,disk);r['input_sha256']=sha(disk)
  binary=out/'edge-tests';run(['g++','-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti','-DOK_ARCH_TARGET_X86_64','-I'+str(src/'include'),src/'tests/host/ext4_owned_edge_tests.cpp',src/'src/fs/ext4_recovery.cpp','-Wl,-l:libext2fs.so.2','-fsanitize=address,undefined','-fno-omit-frame-pointer','-o',binary],'compile')
  run([binary,disk],'hosted');r['status']='passed'
 except Exception as error:r.update(status='failed',error=str(error))
 finally:r['artifact_sha256']={str(f.relative_to(out)):sha(f) for f in sorted(out.rglob('*')) if f.is_file() and f.name!='result.json'};save()
 return 0 if r['status']=='passed' else 1
if __name__=='__main__':raise SystemExit(main())
