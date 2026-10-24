#!/usr/bin/env python3
"""Retain hosted alternate-stack delivery transaction tests and exact inputs."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(exist_ok=False);src=out/'source'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
record={'scope':'Author hosted x86_64/AArch64 alternate-stack delivery transaction; no runtime or guest execution','status':'preparing','files':{},'commands':[],'guest_execution':False,'hardware_isolation':False,'smp':False,'sanitizer':True,'leak_sanitizer':False}
inputs=('include/ok/core/types.hpp','include/ok/abi/linux_user.hpp','include/ok/abi/linux_profile_data.hpp',
 'include/ok/arch/execution.hpp','include/ok/arch/kernel_entry_stack.hpp',
 'include/ok/sched/signal_mask.hpp','include/ok/sched/signal_action.hpp','include/ok/sched/pending_signals.hpp', 'include/ok/sched/child_exit.hpp', 'include/ok/user/native_signal_info.hpp',
 'include/ok/user/native_signal_mask.hpp','include/ok/user/native_signal_action.hpp',
 'include/ok/user/native_signal_delivery.hpp','include/ok/sched/signal_altstack.hpp','include/ok/user/native_signal_altstack.hpp',
 'include/ok/user/native_signal_frame_aarch64.hpp','include/ok/user/native_signal_delivery_aarch64.hpp','include/ok/user/native_signal_frame_x86_64.hpp','include/ok/user/native_signal_delivery_x86_64.hpp',
 'tests/host/native_signal_altstack_delivery_tests.cpp','scripts/test_native_signal_altstack_delivery.py')
for name in inputs:
 p=root/name;q=src/name;q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,q);record['files'][name]=sha(q)
project='''set_project("independent-native-signal-delivery-x86_64")
set_languages("c++23")
target("native-signal-delivery-x86_64")
set_kind("binary")
set_targetdir("artifacts")
add_files("tests/host/native_signal_altstack_delivery_tests.cpp")
add_includedirs("include")
add_defines("OK_ARCH_TARGET_X86_64")
add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-pie",{force=true})
add_ldflags("-fsanitize=address,undefined","-no-pie",{force=true})
target_end()
'''
(src/'xmake.lua').write_text(project);record['files']['xmake.lua']=sha(src/'xmake.lua')
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','XMAKE_GLOBALDIR':str(out/'xmake-global')}
record['environment']={k:env[k] for k in ['ASAN_OPTIONS','UBSAN_OPTIONS']};record['tools']={}
for name in ['xmake','g++']:
 p=Path(shutil.which(name)).resolve();record['tools'][str(p)]=sha(p)
def save():(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
def run(argv,label):
 with (out/(label+'.log')).open('w') as f:r=subprocess.run(list(map(str,argv)),cwd=src,env=env,stdout=f,stderr=subprocess.STDOUT)
 record['commands'].append({'argv':list(map(str,argv)),'returncode':r.returncode,'log':label+'.log','sha256':sha(out/(label+'.log'))});save()
 if r.returncode:raise RuntimeError(label+' failed')
try:
 run(['xmake','f','-P',src,'-c','-y','-o',out/'build','--ccache=n'],'configure')
 run(['xmake','build','-P',src,'-j2','-vD'],'build')
 run([src/'artifacts/native-signal-delivery-x86_64'],'execute')
 found=re.search(r'checks=(\d+) hosted=1 guest=0 smp=0',(out/'execute.log').read_text());assert found
 record['checks']=int(found.group(1))
 for name,wanted in record['files'].items():assert sha(src/name)==wanted,name
 record['current_source_deltas']=[name for name in inputs if sha(root/name)!=record['files'][name]]
 record['binary_sha256']=sha(src/'artifacts/native-signal-delivery-x86_64');record['status']='passed'
except BaseException as error:record.update(status='failed',error=repr(error));save();raise
save();print(out/'result.json')
