"""Retain hosted chmod service/provider evidence independently of guests."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--oracle',type=Path,required=True);a=p.parse_args()
kernel=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
source=out/'input'
for name in ('include','src','tests/host'):
 shutil.copytree(kernel/name,source/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
shutil.copy2(__file__,source/'test_linux_chmod_service.py')
shared=['src/sched/scheduler.cpp','src/sched/process.cpp','src/posix/posix.cpp','src/posix/fd.cpp','src/posix/path.cpp','src/posix/process.cpp','src/fs/vfs.cpp','src/driver/driver.cpp','src/smp/smp.cpp','src/arch/arch.cpp','src/syscall/linux.cpp','src/arch/x86_64/ops.cpp']
suites={'service':['tests/host/chmod_service_tests.cpp','src/fs/ext4_fs.cpp','tests/host/ext4_realtime_model.cpp',*shared], 'initial':['tests/host/initial_child_tests.cpp',*shared], 'checked':['tests/host/checked_vfs_tests.cpp',*shared]}
project=['set_project("chmod-service-hosted")','set_languages("cxx23")']
for name,files in suites.items():
 project += [f'target("chmod-{name}")','set_kind("binary")','add_includedirs("include")','add_defines("OK_ARCH_TARGET_X86_64")','add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror","-ffunction-sections","-fdata-sections",{force=true})','add_ldflags("-Wl,--gc-sections",{force=true})']
 project += ['add_files('+json.dumps(f)+')' for f in files]
 if a.sanitize:project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})','add_ldflags("-fsanitize=address,undefined",{force=true})']
 project += ['target_end()']
(source/'xmake.lua').write_text('\n'.join(project)+'\n')
inputs={str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()}
compiler=Path('/usr/bin/g++').resolve();xmake=Path(shutil.which('xmake')).resolve();cc1=Path(subprocess.check_output([compiler,'-print-prog-name=cc1plus'],text=True).strip()).resolve()
tools={str(p):sha(p) for p in (compiler,xmake,cc1)}
r=dict(status='running',scope='Hosted checked chmod provider/common PosixService/call-adapter composition plus retained initial-child and checked-VFS regressions; no guest/dispatcher/SDK',sanitized=a.sanitize,source_sha256=inputs,tools=tools,commands=[],images={},binaries={})
def save():(out/'result.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label):
 log=out/(label+'.log');cmd=list(map(str,cmd))
 with log.open('x') as stream:done=subprocess.run(cmd,cwd=source,env={**os.environ,'XMAKE_ROOT':'y','XMAKE_GLOBALDIR':str(out/'xmake-global'),'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'},stdout=stream,stderr=subprocess.STDOUT)
 r['commands'].append(dict(label=label,argv=cmd,returncode=done.returncode,log_sha256=sha(log)));save()
 if done.returncode:raise RuntimeError(str(log))
 return log.read_text()
save()
try:
 run([xmake,'f','-P',source,'-c','-y','-m','release','--ccache=n','--cxx='+str(compiler),'--ld='+str(compiler)],'configure')
 run([xmake,'build','-P',source,'-j2','-v'],'build')
 for suite in suites:
  binary=source/'build/linux/x86_64/release'/('chmod-'+suite);r['binaries'][suite]=dict(path=str(binary),sha256=sha(binary))
  if suite!='service':run([binary],suite);continue
  for block in (1024,2048,4096):
   for stride in (128,256):
    label=f'{block}-{stride}';original=a.oracle.resolve()/label/'input.img';image=out/(label+'.img');shutil.copy2(original,image);h=sha(image);r['images'][str(image)]=h
    log=run([binary,image],label)
    assert 'profiles=10 guest=0 dispatcher=0' in log
    assert sha(image)==sha(original)==h
 for name,h in inputs.items():assert sha(source/name)==h,name
 for name,h in tools.items():assert sha(name)==h,name
 r['status']='passed'
except BaseException as e:r.update(status='failed',error=repr(e));save();raise
save();print('PASS hosted chmod service/adapter on six layouts and existing initial/checked regressions')
