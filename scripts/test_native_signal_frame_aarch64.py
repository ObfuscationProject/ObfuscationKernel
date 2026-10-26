#!/usr/bin/env python3
"""Retain hosted AArch64 frame tests plus native freestanding compile evidence."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--toolchain',type=Path,required=True);args=parser.parse_args()
root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(exist_ok=False);src=out/'source'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs=['include/ok/core/types.hpp','include/ok/arch/execution.hpp','include/ok/arch/kernel_entry_stack.hpp','include/ok/abi/linux_user.hpp','include/ok/abi/linux_profile_data.hpp','include/ok/sched/signal_mask.hpp', 'include/ok/sched/child_exit.hpp', 'include/ok/sched/pending_signals.hpp', 'include/ok/user/native_signal_info.hpp','include/ok/user/native_signal_mask.hpp','include/ok/arch/aarch64/signal.hpp','tests/host/native_signal_frame_aarch64_tests.cpp','tests/host/native_signal_frame_aarch64_compile.cpp','tests/fixtures/signals/linux_aarch64_layout.hpp','tests/fixtures/signals/linux_aarch64_layout.json','scripts/test_native_signal_frame_aarch64.py']
record=dict(status='preparing',scope='AArch64 FPSIMD frame hosted models and native compile only',guest_execution=False,files={},commands=[],tools={})
for name in inputs:
 p=src/name;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/name,p);record['files'][name]=sha(p)
layout=json.loads((src/'tests/fixtures/signals/linux_aarch64_layout.json').read_text())
expected_fixture='#pragma once\n// Generated from independent Linux declarations; never from the candidate codec.\nnamespace linux_aarch64_layout {\n'+''.join('inline constexpr unsigned long '+key+' = '+str(value)+';\n' for key,value in layout.items())+'}\n'
if (src/'tests/fixtures/signals/linux_aarch64_layout.hpp').read_text()!=expected_fixture:raise ValueError('Linux layout header differs from retained JSON oracle')
tc=args.toolchain.resolve()/'bin'
project='''set_project("aarch64-signal-frame-model")
set_languages("c++23")
target("aarch64-signal-frame-model")
set_kind("binary")
set_targetdir("artifacts")
add_includedirs("include")
add_files("tests/host/native_signal_frame_aarch64_tests.cpp")
add_cxxflags("-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-pie",{force=true})
add_ldflags("-fsanitize=address,undefined","-no-pie",{force=true})
target_end()
toolchain("frame-aarch64")
set_kind("standalone")
'''
for tool,program in [('cxx','g++'),('ar','ar')]:
 path=tc/('aarch64-elf-'+program);record['tools'][str(path)]=sha(path);project+='set_toolset('+json.dumps(tool)+','+json.dumps(str(path))+')\n'
project+='''toolchain_end()
target("aarch64-signal-frame-compile")
set_kind("static")
set_toolchains("frame-aarch64")
set_targetdir("artifacts")
add_includedirs("include")
add_files("tests/host/native_signal_frame_aarch64_compile.cpp")
add_cxxflags("-O2","-g","-ffreestanding","-fno-exceptions","-frtti","-fno-stack-protector","-fno-pic","-fno-pie","-mno-outline-atomics","-mgeneral-regs-only", "-mstrict-align","-Wall","-Wextra","-Wpedantic","-Werror",{force=true})
target_end()
'''
(src/'xmake.lua').write_text(project);record['files']['xmake.lua']=sha(src/'xmake.lua')
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',XMAKE_GLOBALDIR=str(out/'xmake-global'))
record['environment']={k:env[k] for k in ('ASAN_OPTIONS','UBSAN_OPTIONS')}
for name in ['xmake','g++']:
 p=Path(shutil.which(name)).resolve();record['tools'][str(p)]=sha(p)
def save():(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
try:
 for label,cmd in [('configure',['xmake','f','-P',str(src),'-c','-y','-o',str(out/'build'),'--ccache=n']),('build',['xmake','build','-P',str(src),'-j2','-vD']),('execute',[str(src/'artifacts/aarch64-signal-frame-model')])]:
  with (out/(label+'.log')).open('xb') as log:p=subprocess.run(cmd,cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT)
  record['commands'].append(dict(argv=cmd,returncode=p.returncode,log=label+'.log',sha256=sha(out/(label+'.log'))));save()
  if p.returncode:raise RuntimeError(label+' failed; see retained log')
 for name,wanted in record['files'].items():assert sha(src/name)==wanted,name
 record['model_binary_sha256']=sha(src/'artifacts/aarch64-signal-frame-model')
 record['aarch64_archive_sha256']=sha(src/'artifacts/libaarch64-signal-frame-compile.a')
 record['status']='passed-model-and-native-compile'
except BaseException as error:record.update(status='failed',error=repr(error));save();raise
save();print(out/'result.json')
