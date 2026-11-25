#!/usr/bin/env python3
"""Exercise actual runtime job control: one signal generation core for kill, tkill, group and kernel senders,
stop/continue with their discards, WUNTRACED/WCONTINUED and group waits, SA_NOCLDSTOP, native SIG_DFL/SIG_IGN,
orphaned-group SIGHUP/SIGCONT and owed kernel signals, with hosted checked-copy mappings. The orphaned-group stop
drop also runs per profile, MIPS o32/n64 Linux numbering included, against native and Linux recipients."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,resource,sys

ARCHES=('x86_64',)
# The orphaned-group stop drop on a generic and both MIPS Linux numberings (SIGSTOP 19 and 23).
ORPHAN_STOP_ARCHES={'x86_64':19,'mips':23,'mips64':23}
DIRECTORIES=('core','rescue','driver','fs','gui','interrupt','ipc','memory','net','posix','sched','smp','syscall','user')

def sha(path):
 return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
 parser.add_argument('--arch',choices=('all',*dict.fromkeys((*ARCHES,*ORPHAN_STOP_ARCHES))),default='all');parser.add_argument('--sanitize',action='store_true');parser.add_argument('--cxx',default='g++');args=parser.parse_args()
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);source=out/'source'
 arches=tuple(a for a in ARCHES if args.arch in ('all',a));orphan_arches=tuple(a for a in ORPHAN_STOP_ARCHES if args.arch in ('all',a))
 record=dict(status='preparing',scope='Hosted actual runtime/page/object implementation, selected architecture profiles. Mock trap/copy/mapping backends; no guest, hardware isolation, physical or SMP evidence.',sanitizer=args.sanitize,leak_detection=False,architectures=list(arches),orphan_stop_architectures=list(orphan_arches),source_sha256={},commands=[],rows=[])
 def save():
  p=out/'result.json.tmp';p.write_text(json.dumps(record,indent=2)+'\n');p.replace(out/'result.json')
 def run(argv,label):
  env=dict(os.environ,XMAKE_ROOT='y',XMAKE_GLOBALDIR=str(out/'xmake-global'),XMAKE_TMPDIR=str(out/'xmake-tmp'),ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
  for name in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):env.pop(name,None)
  cmd=list(map(str,argv));result=subprocess.run(cmd,cwd=source,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  log=out/(label+'.log');log.write_bytes(result.stdout);record['commands'].append(dict(argv=cmd,returncode=result.returncode,log=log.name,log_sha256=sha(log)));save()
  if result.returncode:raise RuntimeError(label+' failed; retained '+str(log))
  return result.stdout.decode()
 save()
 try:
  tests={'job-control':'tests/host/native_job_control_tests.cpp','orphan-stop':'tests/host/native_job_control_orphan_stop_tests.cpp'};sources=[]
  for directory in DIRECTORIES:
   sources.extend(str(p.relative_to(root)) for p in sorted((root/'src'/directory).rglob('*.cpp')) if p.name not in ('entry.cpp','kernel_main.cpp','runtime.cpp') and p.parent.name != 'runtime')
  sources.extend(('src/arch/arch.cpp', 'src/arch/terminal_power.cpp'))
  files={root/'tests/host/native_clock_model.hpp',*(root/name for name in tests.values())}|{root/name for name in sources}|{root/'src/user/runtime.cpp',*(root/'src/user/runtime').glob('*.cpp'),Path(__file__).resolve()}
  for base in ('include','src','tests'):
   files.update(p for p in (root/base).rglob('*') if p.is_file() and p.suffix in ('.hpp','.h','.inc'))
  files.update(root/f'src/arch/{arch}/ops.cpp' for arch in (*arches,*orphan_arches))
  for path in sorted(files):
   name=str(path.relative_to(root));digest=sha(path);target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
   if sha(target)!=digest or sha(path)!=digest:raise ValueError('snapshot changed: '+name)
   record['source_sha256'][name]=digest
  cxx=Path(shutil.which(args.cxx) or args.cxx).resolve();xmake=Path(shutil.which('xmake') or '').resolve()
  if not cxx.is_file() or not xmake.is_file():raise ValueError('xmake and compiler required')
  record['tool_sha256']={str(p):sha(p) for p in (cxx,xmake)}
  flags=['-frtti','-Wall','-Wextra','-Wpedantic','-Werror','-ffunction-sections','-fdata-sections'];ld=['-Wl,--gc-sections']
  if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie'];ld+=['-fsanitize=address,undefined','-no-pie']
  lua=['set_project("hosted-job-control")','set_languages("c++23")','set_optimize("fast")','toolchain("poll-host")','    set_kind("standalone")','    set_toolset("cxx", '+json.dumps(str(cxx))+')','    set_toolset("ld", '+json.dumps(str(cxx))+')','toolchain_end()']
  builds=[('job-control',arch) for arch in arches]+[('orphan-stop',arch) for arch in orphan_arches]
  for kind,arch in builds:
   lua += ['target("'+kind+'-'+arch+'")','    set_kind("binary")','    set_toolchains("poll-host")','    add_includedirs("include")','    set_targetdir("bin")','    add_defines("OK_ARCH_TARGET_'+arch.upper()+'")','    add_cxxflags('+', '.join(json.dumps(s) for s in flags)+', {force=true})','    add_ldflags('+', '.join(json.dumps(s) for s in ld)+', {force=true})','    add_files('+', '.join(json.dumps(s) for s in [tests[kind],*sources,f'src/arch/{arch}/ops.cpp'])+')','target_end()']
  (source/'xmake.lua').write_text('\n'.join(lua)+'\n');record['generated_project_sha256']=sha(source/'xmake.lua');record['status']='running';save()
  run([xmake,'f','-P',source,'-c','-y','--ccache=n'],'configure')
  for kind,arch in builds:
   label=arch if kind=='job-control' else kind+'-'+arch
   run([xmake,'build','-P',source,'-j2','-v',kind+'-'+arch],label+'-build')
   binary=source/'bin'/(kind+'-'+arch);output=run([binary],label+'-run')
   if kind=='job-control':
    if not re.search(r'^NATIVE_JOB_CONTROL_RUNTIME_PASS native_calls=[1-9][0-9]* linux_calls=[1-9][0-9]* profiles=10 hosted=1 guest=0$',output,re.M):raise ValueError('missing exact job-control success marker')
    summary='hosted runtime job control (both personalities, ten numberings)'
   else:
    if not re.search(r'^NATIVE_JOB_CONTROL_ORPHAN_STOP_PASS native_stop=19 linux_stop='+str(ORPHAN_STOP_ARCHES[arch])+r' checks=[1-9][0-9]* hosted=1 guest=0$',output,re.M):raise ValueError('missing exact orphan-stop success marker')
    summary='hosted orphaned-group stop drop (native and Linux recipients, Linux SIGSTOP '+str(ORPHAN_STOP_ARCHES[arch])+')'
   record['rows'].append(dict(test=kind,architecture=arch,status='passed',binary_sha256=sha(binary),output=output.strip()));save();print('PASS',arch,summary,flush=True)
  for name,digest in record['source_sha256'].items():
   if sha(source/name)!=digest:raise ValueError('retained source changed: '+name)
  record['status']='passed'
 except BaseException as error:record.update(status='failed',error=repr(error));save();raise
 save()
if __name__=='__main__':main()
