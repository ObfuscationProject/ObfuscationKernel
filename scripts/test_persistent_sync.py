#!/usr/bin/env python3
"""Exercise strict persistent fd sync over an authenticated Linux-journal fixture."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,sys

ARCHES=('x86_64',)
DIRECTORIES=('core','rescue','driver','fs','gui','interrupt','ipc','memory','net','posix','sched','smp','syscall','user')

def sha(path):
 return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--fixture',type=Path,required=True)
 parser.add_argument('--private',action='store_true');parser.add_argument('--arch',choices=('all',*ARCHES),default='all');parser.add_argument('--sanitize',action='store_true');parser.add_argument('--cxx',default='g++');args=parser.parse_args()
 root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);source=out/'source'
 arches=ARCHES if args.arch=='all' else (args.arch,)
 record=dict(status='preparing',scope='Hosted actual runtime/page/object implementation, selected architecture profiles. Mock trap/copy/mapping backends; no guest, hardware isolation, physical or SMP evidence.',sanitizer=args.sanitize,private_access_control_disabled=args.private,leak_detection=False,architectures=list(arches),source_sha256={},commands=[],rows=[])
 def save():
  p=out/'result.json.tmp';p.write_text(json.dumps(record,indent=2)+'\n');p.replace(out/'result.json')
 def run(argv,label):
  env=dict(os.environ,XMAKE_ROOT='y',XMAKE_TMPDIR=str(out/'xmake-tmp'),ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
  for name in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):env.pop(name,None)
  cmd=list(map(str,argv));result=subprocess.run(cmd,cwd=source,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  log=out/(label+'.log');log.write_bytes(result.stdout);record['commands'].append(dict(argv=cmd,returncode=result.returncode,log=log.name,log_sha256=sha(log)));save()
  if result.returncode:raise RuntimeError(label+' failed; retained '+str(log))
  return result.stdout.decode()
 save()
 try:
  sources=['tests/host/persistent_sync_tests.cpp','tests/host/ext4_realtime_model.cpp']
  for directory in DIRECTORIES:
   sources.extend(str(p.relative_to(root)) for p in sorted((root/'src'/directory).rglob('*.cpp')) if p.name not in ('entry.cpp','kernel_main.cpp','runtime.cpp'))
  sources.append('src/arch/arch.cpp')
  sys.path.insert(0,str(root.parent/'ObfuscationOS/tools/jbd2'))
  from admit_linux_fixture import inspect,header_for
  from transaction_fixture import clean_capture
  fixture=args.fixture.resolve();capture=json.loads((fixture/'capture-manifest.json').read_text());data=(fixture/'linux-dirty-capture.img').read_bytes()
  if hashlib.sha256(data).hexdigest()!=capture['capture_sha256']:raise ValueError('captured image hash mismatch')
  facts=inspect(data,True)
  if json.loads(json.dumps(facts))!=json.loads((fixture/'admission.json').read_text()) or header_for(facts)!=(fixture/'linux_fixture.hpp').read_text():raise ValueError('captured fixture admission/header mismatch')
  clean=out/'clean-input.img';clean.write_bytes(clean_capture(data,facts));record['fixture']={'path':str(fixture),'capture_sha256':capture['capture_sha256'],'clean_sha256':sha(clean),'linux_source_provenance':capture['linux_source_provenance']}
  helper_names=('admit_linux_fixture.py','transaction_fixture.py','test_completion.py','native_support.py')
  record['fixture']['helper_sha256']={}
  for name in helper_names:
   helper=root.parent/'ObfuscationOS/tools/jbd2'/name;target=out/'fixture-helpers'/name;target.parent.mkdir(exist_ok=True);shutil.copy2(helper,target);record['fixture']['helper_sha256'][name]=sha(target)
   if sha(helper)!=sha(target):raise ValueError('fixture helper changed: '+name)
  for name in ('capture-manifest.json','admission.json','linux_fixture.hpp'):shutil.copy2(fixture/name,out/name)
  files={root/'tests/host/native_clock_model.hpp'}|{root/name for name in sources}|{root/'src/user/runtime.cpp',Path(__file__).resolve()}
  for base in ('include','src','tests'):
   files.update(p for p in (root/base).rglob('*') if p.is_file() and p.suffix in ('.hpp','.h','.inc'))
  files.update(root/f'src/arch/{arch}/ops.cpp' for arch in ARCHES)
  for path in sorted(files):
   name=str(path.relative_to(root));digest=sha(path);target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
   if sha(target)!=digest or sha(path)!=digest:raise ValueError('snapshot changed: '+name)
   record['source_sha256'][name]=digest
  cxx=Path(shutil.which(args.cxx) or args.cxx).resolve();xmake=Path(shutil.which('xmake') or '').resolve()
  if not cxx.is_file() or not xmake.is_file():raise ValueError('xmake and compiler required')
  record['tool_sha256']={str(p):sha(p) for p in (cxx,xmake)}
  flags=['-frtti','-Wall','-Wextra','-Wpedantic','-Werror','-ffunction-sections','-fdata-sections'];ld=['-Wl,--gc-sections','-Wl,-l:libext2fs.so.2','-pthread']
  if args.private:flags+=['-fno-access-control','-DOK_PERSISTENT_SYNC_PRIVATE_TESTS']
  if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie'];ld+=['-fsanitize=address,undefined','-no-pie']
  lua=['set_project("hosted-persistent-sync")','set_languages("c++23")','set_optimize("fast")','toolchain("poll-host")','    set_kind("standalone")','    set_toolset("cxx", '+json.dumps(str(cxx))+')','    set_toolset("ld", '+json.dumps(str(cxx))+')','toolchain_end()']
  for arch in arches:
   lua += ['target("persistent-sync-'+arch+'")','    set_kind("binary")','    set_toolchains("poll-host")','    add_includedirs("include")','    set_targetdir("bin")','    add_defines("OK_ARCH_TARGET_'+arch.upper()+'")','    add_cxxflags('+', '.join(json.dumps(s) for s in flags)+', {force=true})','    add_ldflags('+', '.join(json.dumps(s) for s in ld)+', {force=true})','    add_files('+', '.join(json.dumps(s) for s in [*sources,f'src/arch/{arch}/ops.cpp'])+')','target_end()']
  (source/'xmake.lua').write_text('\n'.join(lua)+'\n');record['generated_project_sha256']=sha(source/'xmake.lua');record['status']='running';save()
  run([xmake,'f','-P',source,'-c','-y','--ccache=n'],'configure')
  for arch in arches:
   run([xmake,'build','-P',source,'-j2','-v','persistent-sync-'+arch],arch+'-build')
   binary=source/'bin'/('persistent-sync-'+arch);groups=('unqualified','persistent','failures')+(('adversarial',) if args.private else ());outputs={}
   for group in groups:
    output=run([binary,clean,group],arch+'-'+group+'-run')
    if 'PERSISTENT_SYNC_GROUP_PASS group='+group+' guest=0 physical=0' not in output:raise ValueError('missing exact persistent sync group marker')
    outputs[group]=output.strip()
   record['rows'].append(dict(architecture=arch,status='passed',binary_sha256=sha(binary),groups=outputs));save();print('PASS',arch,'hosted persistent sync dispatch groups='+','.join(groups),flush=True)
  run(['e2fsck','-fn',clean],'clean-fixture-readonly-fsck')
  if sha(clean)!=record['fixture']['clean_sha256']:raise ValueError('clean fixture changed')
  if sha(fixture/'linux-dirty-capture.img')!=record['fixture']['capture_sha256']:raise ValueError('original capture changed')
  for name,digest in record['source_sha256'].items():
   if sha(source/name)!=digest:raise ValueError('retained source changed: '+name)
  record['status']='passed'
 except BaseException as error:record.update(status='failed',error=repr(error));save();raise
 save()
if __name__=='__main__':main()
