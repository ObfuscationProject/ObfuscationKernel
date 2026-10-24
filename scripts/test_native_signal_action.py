#!/usr/bin/env python3
"""Retain a source-pinned xmake model execution and Linux layout comparison."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(exist_ok=False);src=out/'source'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
record={'scope':'Internal signal actions only; all-ten hosted models, not runtime or delivery','status':'preparing','files':{},'commands':[],'guest_execution':False,'delivery':False}
inputs=('include/ok/core/types.hpp','include/ok/abi/linux_user.hpp','include/ok/abi/linux_profile_data.hpp',
        'include/ok/sched/signal_mask.hpp','include/ok/sched/signal_action.hpp',
        'include/ok/user/native_signal_mask.hpp','include/ok/user/native_signal_action.hpp',
        'tests/host/native_signal_action_tests.cpp','tests/fixtures/signals/linux-action-layouts.json',
        'scripts/test_native_signal_action.py')
for p in [root/name for name in inputs]:
    q=src/p.relative_to(root);q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,q);record['files'][str(q.relative_to(src))]=sha(q)
build_definition='''set_project("internal-signal-actions-model")
set_languages("c++23")
target("native-signal-action-model")
set_kind("binary")
set_targetdir("artifacts")
add_files("tests/host/native_signal_action_tests.cpp")
add_includedirs("include")
add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-pie",{force=true})
add_ldflags("-fsanitize=address,undefined","-no-pie",{force=true})
target_end()
'''
(src/'xmake.lua').write_text(build_definition);record['files']['xmake.lua']=sha(src/'xmake.lua')
oracle=src/'tests/fixtures/signals/linux-action-layouts.json';shutil.copyfile(oracle,out/'linux-action-layouts.json');record['oracle_sha256']=sha(oracle)
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
 run([src/'artifacts/native-signal-action-model'],'execute')
 fields=['bytes','handler_offset','flags_offset','restorer_offset_or_999','mask_offset','flags_bytes'];actual={}
 for line in (out/'execute.log').read_text().splitlines():
  if line.startswith('LAYOUT '):
   _,name,*values=line.split();actual[name]=dict(zip(fields,map(int,values)))
 expected=json.loads(oracle.read_text());assert actual==expected,(actual,expected)
 record['layout_oracle_comparison']='all-ten-equal';record['profiles']=list(actual)
 for name,wanted in record['files'].items():assert sha(src/name)==wanted,name
 record['binary_sha256']=sha(src/'artifacts/native-signal-action-model');record['status']='passed'
except BaseException as error:record.update(status='failed',error=repr(error));save();raise
save();print(out/'result.json')
