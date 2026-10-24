#!/usr/bin/env python3
"""Hosted typed futex origin and n64/PPC publication models; no runtime/guest execution."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--ppc-elf',required=True,type=Path);p.add_argument('--ppc-elf-sha256',required=True);p.add_argument('--targets',nargs='+',choices=('origin','publication'),default=['origin','publication']);args=p.parse_args()
out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False);src=out/'source'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
tool_pins={str(Path(shutil.which(name)).resolve()):sha(Path(shutil.which(name)).resolve()) for name in ('xmake','g++')}
files=list((root/'include').rglob('*.hpp'))+[root/'src/user/native_syscall_origin.hpp',root/'src/user/native_syscall_restart_publication.hpp',root/'src/sched/scheduler.cpp',root/'src/sched/process.cpp']
files += [root/('tests/host/'+n) for n in ('native_futex_origin_tests.cpp','native_futex_restart_publication_tests.cpp')]
files += [root/('tests/fixtures/signals/'+n) for n in ('linux_ppc32_layout.hpp','linux_mips64_n64_layout.hpp')]
record={'runner_sha256':sha(Path(__file__)), 'tools':tool_pins, 'targets':args.targets,'scope':'Hosted owned-table and backend observation models; no native OS execution','status':'preparing','inputs':{},'commands':[]}
for f in files:
 rel=f.relative_to(root);dst=src/rel;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,dst);record['inputs'][str(rel)]=sha(dst)
elf=args.ppc_elf.resolve();assert sha(elf)==args.ppc_elf_sha256,'PPC ELF input hash mismatch'
shutil.copy2(elf,out/'linux.elf');record['linux_elf_sha256']=sha(out/'linux.elf')
flags='"-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-ffunction-sections", "-fdata-sections"'
lua=['set_project("restart-publication-model")','set_languages("c++23")']
for name,source in [('origin','native_futex_origin_tests'),('publication','native_futex_restart_publication_tests')]:
 lua += [f'target("{name}")','set_kind("binary")','set_targetdir("bin")','add_includedirs("include")','add_defines("OK_ARCH_TARGET_X86_64")',f'add_files("tests/host/{source}.cpp")',f'add_cxxflags({flags}, {{force=true}})','add_ldflags("-fsanitize=address,undefined", "-no-pie", "-Wl,--gc-sections", {force=true})']
 lua += ['add_files("src/sched/scheduler.cpp", "src/sched/process.cpp")']
 lua+=['target_end()']
(src/'xmake.lua').write_text('\n'.join(lua)+'\n');record['project_sha256']=sha(src/'xmake.lua')
env=dict(os.environ,XMAKE_ROOT='y',XMAKE_GLOBALDIR=str(out/'xmake-global'),PYTHONDONTWRITEBYTECODE='1',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for n in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):env.pop(n,None)
def save():(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
def run(cmd,label):
 with (out/(label+'.log')).open('wb') as log:r=subprocess.run(list(map(str,cmd)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT)
 record['commands'].append({'argv':list(map(str,cmd)),'returncode':r.returncode,'log':label+'.log','sha256':sha(out/(label+'.log'))});save()
 if r.returncode:raise RuntimeError(label+' failed')
try:
 record['status']='running';save()
 run(['xmake','f','-P',src,'-c','-y','--ccache=n'],'configure')
 for name in args.targets:
  run(['xmake','build','-P',src,'-j2','-v',name],name+'-build')
  run([src/'bin'/name,*([out/'linux.elf'] if name=='publication' else [])],name+'-run')
  print((out/(name+'-run.log')).read_text(),flush=True)
 record['status']='passed'
except BaseException as e:record.update(status='failed',error=repr(e));save();raise
save()
