#!/usr/bin/env python3
"""Retain and execute real runtime/page/object code with hosted trap backends."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,sys,struct

ARCHES=('i386','x86_64','aarch64','arm32','rv64','rv32','loongarch64','mips','mips64','ppc')
DIRECTORIES=('core','rescue','driver','fs','gui','interrupt','ipc','memory','net','posix','sched','smp','syscall','user')

def sha(path):
 return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
 parser.add_argument('--arch',choices=('x86_64',),default='x86_64');parser.add_argument('--sanitize',action='store_true');parser.add_argument('--cxx',default='g++');args=parser.parse_args()
 root=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=False);source=out/'source'
 arches=(args.arch,)
 record=dict(status='preparing',scope='Hosted actual runtime/page/object implementation, selected architecture profiles. Mock trap/copy/mapping backends; no guest, hardware isolation, physical or SMP evidence.',sanitizer=args.sanitize,leak_detection=False,architectures=list(arches),source_sha256={},commands=[],rows=[])
 def save():
  p=out/'result.json.tmp';p.write_text(json.dumps(record,indent=2)+'\n');p.replace(out/'result.json')
 def run(argv,label):
  env=dict(os.environ,XMAKE_ROOT='y',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
  for name in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):env.pop(name,None)
  cmd=list(map(str,argv));result=subprocess.run(cmd,cwd=source,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  log=out/(label+'.log');log.write_bytes(result.stdout);record['commands'].append(dict(argv=cmd,returncode=result.returncode,log=log.name,log_sha256=sha(log)));save()
  if result.returncode:raise RuntimeError(label+' failed; retained '+str(log))
  return result.stdout.decode()
 save()
 try:
  sources=['tests/host/native_signal_exec_tests.cpp']
  for directory in DIRECTORIES:
   sources.extend(str(p.relative_to(root)) for p in sorted((root/'src'/directory).rglob('*.cpp')) if p.name not in ('entry.cpp','kernel_main.cpp','runtime.cpp') and p.parent.name != 'runtime')
  sources.append('src/arch/arch.cpp')
  files={root/'tests/host/native_clock_model.hpp'}|{root/name for name in sources}|{root/'src/user/runtime.cpp',*(root/'src/user/runtime').glob('*.cpp'),Path(__file__).resolve()}
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
  flags=['-frtti','-Wall','-Wextra','-Wpedantic','-Werror','-ffunction-sections','-fdata-sections'];ld=['-Wl,--gc-sections']
  if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie'];ld+=['-fsanitize=address,undefined','-no-pie']
  lua=['set_project("hosted-runtime-native-signal-exec")','set_languages("c++23")','set_optimize("fast")','toolchain("poll-host")','    set_kind("standalone")','    set_toolset("cxx", '+json.dumps(str(cxx))+')','    set_toolset("ld", '+json.dumps(str(cxx))+')','toolchain_end()']
  for arch in arches:
   lua += ['target("native-signal-exec-'+arch+'")','    set_kind("binary")','    set_toolchains("poll-host")','    add_includedirs("include")','    set_targetdir("bin")','    add_defines("OK_ARCH_TARGET_'+arch.upper()+'")','    add_cxxflags('+', '.join(json.dumps(s) for s in flags)+', {force=true})','    add_ldflags('+', '.join(json.dumps(s) for s in ld)+', {force=true})','    add_files('+', '.join(json.dumps(s) for s in [*sources,f'src/arch/{arch}/ops.cpp'])+')','target_end()']
  (source/'xmake.lua').write_text('\n'.join(lua)+'\n');record['generated_project_sha256']=sha(source/'xmake.lua');record['status']='running';save()
  run([xmake,'f','-P',source,'-c','-y','--ccache=n'],'configure')
  # Minimal ELF64 loader input, never executed by the mock entry backend.
  fixture=bytearray(4112);fixture[:16]=b'\x7fELF\x02\x01\x01\x03'+bytes(8)
  struct.pack_into('<HHIQQQIHHHHHH',fixture,16,2,62,1,0x40000100,64,0,0,64,56,2,0,0,0)
  struct.pack_into('<IIQQQQQQ',fixture,64,1,5,0,0x40000000,0x40000000,512,4096,4096)
  struct.pack_into('<IIQQQQQQ',fixture,120,1,6,4096,0x40001000,0x40001000,16,4096,4096)
  fixture[256]=0xc3
  (out/'input.elf').write_bytes(fixture);record['loader_fixture_sha256']=sha(out/'input.elf');save()
  for arch in arches:
   run([xmake,'build','-P',source,'-j2','-v','native-signal-exec-'+arch],arch+'-build')
   binary=source/'bin'/('native-signal-exec-'+arch)
   for case in ('lookup','normal-new','normal-old','fatal-new','fatal-old'):
    output=run([binary,out/'input.elf',case],arch+'-'+case+'-run')
    if not output.startswith('PASS SDK28 ') or 'guest=0' not in output:raise ValueError('missing hosted completion marker')
    record['rows'].append(dict(architecture=arch,case=case,status='passed',binary_sha256=sha(binary),output=output.strip()));save();print('PASS',arch,case,'hosted native signal exec',flush=True)
  for name,digest in record['source_sha256'].items():
   if sha(source/name)!=digest:raise ValueError('retained source changed: '+name)
  record['status']='passed'
 except BaseException as error:record.update(status='failed',error=repr(error));save();raise
 save()
if __name__=='__main__':main()
